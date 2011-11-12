/*
 * Copyright 2011 Shinpei Kato
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "gdev_conf.h"
#include "gdev_drv.h"
#include "nouveau_drv.h"
#include "pscnv_chan.h"
#include "pscnv_fifo.h"
#include "pscnv_gem.h"
#include "pscnv_ioctl.h"
#include "pscnv_mem.h"
#include "pscnv_vm.h"

extern uint32_t *nvc0_fifo_ctrl_ptr(struct drm_device *, struct pscnv_chan *);

/* allocate a new memory object. */
static inline gdev_mem_t *__gdev_mem_alloc
(gdev_vas_t *vas, uint64_t size, uint32_t flags)
{
	gdev_mem_t *mem;
	gdev_device_t *gdev = vas->gdev;
	struct drm_device *drm = gdev->drm;
	struct pscnv_vspace *vspace = vas->pvas;
	struct pscnv_bo *bo;
	struct pscnv_mm_node *mm;

	if (!(mem = kzalloc(sizeof(*mem), GFP_KERNEL)))
		goto fail_mem;

	if (!(bo = pscnv_mem_alloc(drm, size, flags, 0, 0)))
		goto fail_bo;

	if (pscnv_vspace_map(vspace, bo, GDEV_VAS_USER_START, 
						 GDEV_VAS_USER_END, 0, &mm))
		goto fail_map;

	mem->vas = vas;
	mem->bo = bo;
	mem->addr = mm->start;
	if (flags & PSCNV_GEM_SYSRAM_SNOOP) {
		if (size > PAGE_SIZE)
			mem->map = vmap(bo->pages, bo->size >> PAGE_SHIFT, 0, PAGE_KERNEL);
		else
			mem->map = kmap(bo->pages[0]);
	}
	else
		mem->map = NULL;

	__gdev_list_init(&mem->list_entry, (void *)mem);

	return mem;

fail_map:
	GDEV_PRINT("Failed to map VAS.\n");
	pscnv_mem_free(bo);
fail_bo:
	GDEV_PRINT("Failed to allocate buffer object.\n");
	kfree(mem);
fail_mem:
	return NULL;
}

/* free the specified memory object. */
static inline void __gdev_mem_free(gdev_mem_t *mem)
{
	gdev_vas_t *vas = mem->vas;
	struct pscnv_vspace *vspace = vas->pvas;
	struct pscnv_bo *bo = mem->bo;

	if (mem->map) {
		if (bo->size > PAGE_SIZE)
			vunmap(mem->map);
		else
			kunmap(mem->map);
	}
	pscnv_vspace_unmap(vspace, mem->addr);
	pscnv_mem_free(mem->bo);
	kfree(mem);
}

/* initialize the compute engine. */
int gdev_compute_init(gdev_device_t *gdev)
{
	struct drm_device *drm = gdev->drm;
	struct drm_nouveau_private *priv = drm->dev_private;
	uint32_t chipset = priv->chipset;

    switch (chipset & 0xf0) {
    case 0xC0:
		nvc0_compute_setup(gdev);
        break;
    case 0x50:
    case 0x80:
    case 0x90:
    case 0xA0:
		/* TODO: create the compute and m2mf subchannels! */
		GDEV_PRINT("NV%x not supported.\n", chipset);
		return -EINVAL;
    default:
		GDEV_PRINT("NV%x not supported.\n", chipset);
		return -EINVAL;
    }

	return 0;
}

/* query a piece of the device-specific information. */
int gdev_info_query(gdev_device_t *gdev, uint32_t type, uint32_t *result)
{
	struct drm_device *drm = gdev->drm;
	struct drm_nouveau_private *priv = drm->dev_private;
	struct drm_pscnv_getparam getparam;
	uint32_t chipset = priv->chipset;

	switch (type) {
	case GDEV_QUERY_NVIDIA_MP_COUNT:
		if ((chipset & 0xf0) != 0xc0)
			return -EINVAL;
		getparam.param = PSCNV_GETPARAM_MP_COUNT;
		pscnv_ioctl_getparam(drm, &getparam, NULL);
		*result = getparam.value;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* open a new Gdev object associated with the specified device. */
gdev_device_t *gdev_dev_open(int devnum)
{
	gdev_device_t *gdev = &gdrv.gdev[devnum];
	gdev->use++;
	return gdev;
}

/* close the specified Gdev object. */
void gdev_dev_close(gdev_device_t *gdev)
{
	gdev->use--;
}

/* allocate a new virual address space object. */
gdev_vas_t *gdev_vas_new(gdev_device_t *gdev, uint64_t size)
{
	gdev_vas_t *vas;
	struct drm_device *drm = gdev->drm;
	struct pscnv_vspace *vspace;

	if (!(vas = kzalloc(sizeof(*vas), GFP_KERNEL)))
		goto fail_vas;

	if (!(vspace = pscnv_vspace_new(drm, size, 0, 0)))
		goto fail_vspace;

	/* we don't need vspace->filp in Gdev.  */
	vspace->filp = NULL;

	vas->gdev = gdev;
	vas->pvas = vspace; /* driver private object. */

	__gdev_list_init(&vas->memlist, NULL);

	return vas;

fail_vspace:
	kfree(vas);
fail_vas:
	return NULL;
}

/* free the specified virtual address space object. */
void gdev_vas_free(gdev_vas_t *vas)
{
	struct pscnv_vspace *vspace = vas->pvas;

	vspace->filp = NULL;
	pscnv_vspace_unref(vspace);

	kfree(vas);
}

/* create a new GPU context object. */
gdev_ctx_t *gdev_ctx_new(gdev_device_t *gdev, gdev_vas_t *vas)
{
	gdev_ctx_t *ctx;
	struct gdev_compute *compute = gdev->compute;
	struct drm_device *drm = gdev->drm;
	struct drm_nouveau_private *priv = drm->dev_private;
	uint32_t chipset = priv->chipset;
	struct pscnv_vspace *vspace = vas->pvas; 
	struct pscnv_chan *chan;
	struct pscnv_bo *ib_bo, *pb_bo, *fence_bo;
	struct pscnv_mm_node *ib_mm, *pb_mm, *fence_mm;
	int i, ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		goto fail_ctx;

	chan = pscnv_chan_new(drm, vspace, 0);
	if (!chan)
		goto fail_chan;

	/* we don't need chan->filp in Gdev. */
	chan->filp = NULL;

    if ((chipset & 0xf0) != 0xc0) {
		/* TODO: set up vdma here! */
	}

	/* FIFO indirect buffer setup. */
	ctx->fifo.ib_order = 9; /* it's hardcoded. */
	ib_bo = pscnv_mem_alloc(drm, 8 << ctx->fifo.ib_order, 
							PSCNV_GEM_SYSRAM_SNOOP, 0, 0);
	if (!ib_bo) {
		goto fail_ib;
	}
	ret = pscnv_vspace_map(vspace, ib_bo, GDEV_VAS_USER_START, 
						   GDEV_VAS_USER_END, 0, &ib_mm);
	if (ret)
		goto fail_ibmap;
	ctx->fifo.ib_map = vmap(ib_bo->pages, ib_bo->size >> PAGE_SHIFT, 0, 
							PAGE_KERNEL);
	ctx->fifo.ib_bo = ib_bo;
	ctx->fifo.ib_base = ib_mm->start;
	ctx->fifo.ib_mask = (1 << ctx->fifo.ib_order) - 1;
	ctx->fifo.ib_put = ctx->fifo.ib_get = 0;

	/* FIFO push buffer setup. */
	ctx->fifo.pb_order = 20; /* it's hardcoded. */
	pb_bo = pscnv_mem_alloc(drm, 1 << ctx->fifo.pb_order, 
							PSCNV_GEM_SYSRAM_SNOOP, 0, 0);
	if (!pb_bo)
		goto fail_pb;
	ret = pscnv_vspace_map(vspace, pb_bo, GDEV_VAS_USER_START, 
						   GDEV_VAS_USER_END, 0, &pb_mm);
	if (ret)
		goto fail_pbmap;
	ctx->fifo.pb_map = vmap(pb_bo->pages, pb_bo->size >> PAGE_SHIFT, 0, 
							PAGE_KERNEL);
	ctx->fifo.pb_bo = pb_bo;
	ctx->fifo.pb_base = pb_mm->start;
	ctx->fifo.pb_mask = (1 << ctx->fifo.pb_order) - 1;
	ctx->fifo.pb_size = (1 << ctx->fifo.pb_order);
	ctx->fifo.pb_pos = ctx->fifo.pb_put = ctx->fifo.pb_get = 0;

	/* FIFO init. */
	ret = priv->fifo->chan_init_ib(chan, 0, 0, 1, 
								   ctx->fifo.ib_base, ctx->fifo.ib_order);
	if (ret)
		goto fail_fifo_init;

	/* FIFO command queue registers. */
	switch (chipset & 0xf0) {
	case 0xc0:
		ctx->fifo.regs = nvc0_fifo_ctrl_ptr(drm, chan);
		break;
	default:
		goto fail_fifo_reg;
	}

	/* fences init. */
	fence_bo = pscnv_mem_alloc(drm, PAGE_SIZE, PSCNV_GEM_SYSRAM_SNOOP, 0, 0);
	if (!fence_bo)
		goto fail_fence_alloc;
	ret = pscnv_vspace_map(vspace, fence_bo, GDEV_VAS_USER_START, 
						   GDEV_VAS_USER_END, 0, &fence_mm);
	if (ret)
		goto fail_fence_map;
	ctx->fence.bo = fence_bo;
	ctx->fence.map = kmap(fence_bo->pages[0]); /* assume < PAGE_SIZE */
	ctx->fence.addr = fence_mm->start;
	for (i = 0; i < GDEV_FENCE_COUNT; i++) {
		ctx->fence.sequence[i] = 0;
	}

	ctx->vas = vas;
	ctx->pctx = chan;

	/* initialize the channel. */
	compute->init(ctx);

	return ctx;
	
fail_fence_map:
	pscnv_mem_free(fence_bo);
fail_fence_alloc:
fail_fifo_reg:
fail_fifo_init:
	vunmap(ctx->fifo.pb_map);
	pscnv_vspace_unmap(vspace, pb_mm->start);
fail_pbmap:
	pscnv_mem_free(pb_bo);
fail_pb:
	vunmap(ctx->fifo.ib_map);
	pscnv_vspace_unmap(vspace, ib_mm->start);
fail_ibmap:
	pscnv_mem_free(ib_bo);
fail_ib:
	chan->filp = NULL;
	pscnv_chan_unref(chan);
fail_chan:
	kfree(ctx);
fail_ctx:
	return NULL;
}

/* destroy the specified GPU context object. */
void gdev_ctx_free(gdev_ctx_t *ctx)
{
	gdev_vas_t *vas = ctx->vas; 
	struct pscnv_vspace *vspace = vas->pvas;
	struct pscnv_chan *chan = ctx->pctx;
	struct pscnv_bo *fence_bo = ctx->fence.bo;

	kunmap(fence_bo->pages[0]);
	pscnv_vspace_unmap(vspace, ctx->fence.addr);
	pscnv_mem_free((struct pscnv_bo *)ctx->fence.bo);
	vunmap(ctx->fifo.pb_map);
	pscnv_vspace_unmap(vspace, ctx->fifo.pb_base);
	pscnv_mem_free((struct pscnv_bo *)ctx->fifo.pb_bo);
	vunmap(ctx->fifo.ib_map);
	pscnv_vspace_unmap(vspace, ctx->fifo.ib_base);
	pscnv_mem_free((struct pscnv_bo *)ctx->fifo.ib_bo);

	chan->filp = NULL;
	pscnv_chan_unref(chan);

	kfree(ctx);
}

/* allocate a new device memory object. */
gdev_mem_t *gdev_malloc_device(gdev_vas_t *vas, uint64_t size)
{
	return __gdev_mem_alloc(vas, size, PSCNV_GEM_VRAM_SMALL);
}

/* free the specified device memory object. */
void gdev_free_device(gdev_mem_t *mem)
{
	return __gdev_mem_free(mem);
}

/* allocate a new DMA (host) memory object. */
gdev_mem_t *gdev_malloc_dma(gdev_vas_t *vas, uint64_t size)
{
	return __gdev_mem_alloc(vas, size, PSCNV_GEM_SYSRAM_SNOOP);
}

/* free the specified DMA (host) memory object. */
void gdev_free_dma(gdev_mem_t *mem)
{
	return __gdev_mem_free(mem);
}