/*
 * Copyright © 2006 Intel Corporation
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Xiang Haihao <haihao.xiang@intel.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <assert.h>

#include <pthread.h>
#include <sys/ioctl.h>
#include <X11/Xlibint.h>
#include <fourcc.h>
#include <X11/extensions/Xv.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XvMC.h>
#include <X11/extensions/XvMClib.h>
#include <xf86drm.h>
#include <drm_sarea.h>

#include "I915XvMC.h"
#include "i915_structs.h"
#include "i915_program.h"
#include "intel_batchbuffer.h"
#include "xf86dri.h"
#include "driDrawable.h"

#define _STATIC_ static

#define SAREAPTR(ctx) ((drmI830Sarea *)                     \
                       (((CARD8 *)(ctx)->sarea_address) +   \
                        (ctx)->sarea_priv_offset))

#define YOFFSET(surface)        (surface->srf.offset)
#define UOFFSET(surface)        (surface->srf.offset + \
                                 SIZE_Y420(surface->width, surface->height) + \
                                 SIZE_UV420(surface->width, surface->height))
#define VOFFSET(surface)        (surface->srf.offset + \
                                 SIZE_Y420(surface->width, surface->height))

/* Lookup tables to speed common calculations */
_STATIC_ unsigned int mb_bytes[] = {
    000, 128, 128, 256, 128, 256, 256, 384,  // 0
    128, 256, 256, 384, 256, 384, 384, 512,  // 1
    128, 256, 256, 384, 256, 384, 384, 512,  // 10
    256, 384, 384, 512, 384, 512, 512, 640,  // 11
    128, 256, 256, 384, 256, 384, 384, 512,  // 100
    256, 384, 384, 512, 384, 512, 512, 640,  // 101    
    256, 384, 384, 512, 384, 512, 512, 640,  // 110
    384, 512, 512, 640, 512, 640, 640, 768   // 111
};

typedef union {
    short s[4];
    uint  u[2];
} su_t;

_STATIC_ char I915KernelDriverName[] = "i915";
_STATIC_ int error_base;
_STATIC_ int event_base;

_STATIC_ int findOverlap(unsigned int width, unsigned int height,
                       short *dstX, short *dstY,
                       short *srcX, short *srcY, 
                       unsigned short *areaW, unsigned short *areaH)
{
    int w, h;
    unsigned int mWidth, mHeight;

    w = *areaW;
    h = *areaH;

    if ((*dstX >= width) || (*dstY >= height))
        return 1;

    if (*dstX < 0) {
        w += *dstX;
        *srcX -= *dstX;
        *dstX = 0;
    }

    if (*dstY < 0) {
        h += *dstY;
        *srcY -= *dstY;
        *dstY = 0;
    }

    if ((w <= 0) || ((h <= 0)))
        return 1;

    mWidth = width - *dstX;
    mHeight = height - *dstY;
    *areaW = (w <= mWidth) ? w : mWidth;
    *areaH = (h <= mHeight) ? h : mHeight;
    return 0;
}

_STATIC_ __inline__ void renderError(void) 
{
    XVMC_ERR("Invalid Macroblock Parameters found.");
}

_STATIC_ void I915XvMCContendedLock(i915XvMCContext *pI915XvMC, drmLockFlags flags)
{
    drmGetLock(pI915XvMC->fd, pI915XvMC->hHWContext, flags);
}

#define SET_BLOCKED_SIGSET(pI915XvMC)   do {    \
        sigset_t bl_mask;                       \
        sigfillset(&bl_mask);           \
        sigdelset(&bl_mask, SIGFPE);    \
        sigdelset(&bl_mask, SIGILL);    \
        sigdelset(&bl_mask, SIGSEGV);   \
        sigdelset(&bl_mask, SIGBUS);    \
        sigdelset(&bl_mask, SIGKILL);   \
        pthread_sigmask(SIG_SETMASK, &bl_mask, &pI915XvMC->sa_mask); \
    } while (0)

#define RESTORE_BLOCKED_SIGSET(pI915XvMC) do {    \
        pthread_sigmask(SIG_SETMASK, &pI915XvMC->sa_mask, NULL); \
    } while (0)

#define PPTHREAD_MUTEX_LOCK(pI915XvMC) do {             \
        SET_BLOCKED_SIGSET(pI915XvMC);                  \
        pthread_mutex_lock(&pI915XvMC->ctxmutex);       \
    } while (0)

#define PPTHREAD_MUTEX_UNLOCK(pI915XvMC) do {           \
        pthread_mutex_unlock(&pI915XvMC->ctxmutex);     \
        RESTORE_BLOCKED_SIGSET(pI915XvMC);              \
    } while (0)

/* Lock the hardware and validate our state.
 */
_STATIC_ void LOCK_HARDWARE(i915XvMCContext  *pI915XvMC)
{
    char __ret = 0;

    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    assert(!pI915XvMC->locked);

    DRM_CAS(pI915XvMC->driHwLock, pI915XvMC->hHWContext,
            (DRM_LOCK_HELD|pI915XvMC->hHWContext), __ret);

    if (__ret)
        I915XvMCContendedLock(pI915XvMC, 0);

    pI915XvMC->locked = 1;
}

/* Unlock the hardware using the global current context
 */
_STATIC_ void UNLOCK_HARDWARE(i915XvMCContext *pI915XvMC)
{
    pI915XvMC->locked = 0;
    DRM_UNLOCK(pI915XvMC->fd, pI915XvMC->driHwLock, 
               pI915XvMC->hHWContext);
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
}

_STATIC_ void i915_flush(i915XvMCContext *pI915XvMC, int map, int render)
{
    struct i915_mi_flush mi_flush;

    memset(&mi_flush, 0, sizeof(mi_flush));
    mi_flush.dw0.type = CMD_MI;
    mi_flush.dw0.opcode = OPC_MI_FLUSH;
    mi_flush.dw0.map_cache_invalidate = map;
    mi_flush.dw0.render_cache_flush_inhibit = render;

    intelBatchbufferData(pI915XvMC, &mi_flush, sizeof(mi_flush), 0);
}

/* for MC picture rendering */
_STATIC_ void i915_mc_static_indirect_state_buffer(XvMCContext *context, 
                                                   XvMCSurface *surface,
                                                   unsigned int picture_structure,
                                                   unsigned int flags,
                                                   unsigned int picture_coding_type)
{
    struct i915_3dstate_buffer_info *buffer_info;
    struct i915_3dstate_dest_buffer_variables *dest_buffer_variables;
    struct i915_3dstate_dest_buffer_variables_mpeg *dest_buffer_variables_mpeg;
    i915XvMCSurface *pI915Surface = (i915XvMCSurface *)surface->privData;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    unsigned int w = surface->width, h = surface->height;

    /* 3DSTATE_BUFFER_INFO */
    /* DEST Y */
    buffer_info = (struct i915_3dstate_buffer_info *)pI915XvMC->sis.map;
    memset(buffer_info, 0, sizeof(*buffer_info));
    buffer_info->dw0.type = CMD_3D;
    buffer_info->dw0.opcode = OPC_3DSTATE_BUFFER_INFO;
    buffer_info->dw0.length = 1;
    buffer_info->dw1.aux_id = 0;
    buffer_info->dw1.buffer_id = BUFFERID_COLOR_BACK;
    buffer_info->dw1.fence_regs = 0;    /* disabled */ /* FIXME: tiled y for performance */
    buffer_info->dw1.tiled_surface = 0; /* linear */
    buffer_info->dw1.walk = TILEWALK_XMAJOR;
    buffer_info->dw1.pitch = (pI915Surface->yStride >> 2);      /* in DWords */
    buffer_info->dw2.base_address = (YOFFSET(pI915Surface) >> 2);    /* starting DWORD address */

    /* DEST U */
    ++buffer_info;
    memset(buffer_info, 0, sizeof(*buffer_info));
    buffer_info->dw0.type = CMD_3D;
    buffer_info->dw0.opcode = OPC_3DSTATE_BUFFER_INFO;
    buffer_info->dw0.length = 1;
    buffer_info->dw1.aux_id = 0;
    buffer_info->dw1.buffer_id = BUFFERID_COLOR_AUX;
    buffer_info->dw1.fence_regs = 0;
    buffer_info->dw1.tiled_surface = 0;
    buffer_info->dw1.walk = TILEWALK_XMAJOR; 
    buffer_info->dw1.pitch = (pI915Surface->uvStride >> 2);      /* in DWords */
    buffer_info->dw2.base_address = (UOFFSET(pI915Surface) >> 2);      /* starting DWORD address */

    /* DEST V */
    ++buffer_info;
    memset(buffer_info, 0, sizeof(*buffer_info));
    buffer_info->dw0.type = CMD_3D;
    buffer_info->dw0.opcode = OPC_3DSTATE_BUFFER_INFO;
    buffer_info->dw0.length = 1;
    buffer_info->dw1.aux_id = 1;
    buffer_info->dw1.buffer_id = BUFFERID_COLOR_AUX;
    buffer_info->dw1.fence_regs = 0;
    buffer_info->dw1.tiled_surface = 0;
    buffer_info->dw1.walk = TILEWALK_XMAJOR; 
    buffer_info->dw1.pitch = (pI915Surface->uvStride >> 2);      /* in Dwords */
    buffer_info->dw2.base_address = (VOFFSET(pI915Surface) >> 2);      /* starting DWORD address */

    /* 3DSTATE_DEST_BUFFER_VARIABLES */
    dest_buffer_variables = (struct i915_3dstate_dest_buffer_variables *)(++buffer_info);
    memset(dest_buffer_variables, 0, sizeof(*dest_buffer_variables));
    dest_buffer_variables->dw0.type = CMD_3D;
    dest_buffer_variables->dw0.opcode = OPC_3DSTATE_DEST_BUFFER_VARIABLES;
    dest_buffer_variables->dw0.length = 0;
    dest_buffer_variables->dw1.dest_v_bias = 8; /* 0.5 */
    dest_buffer_variables->dw1.dest_h_bias = 8; /* 0.5 */
    dest_buffer_variables->dw1.color_fmt = COLORBUFFER_8BIT;
    dest_buffer_variables->dw1.v_ls = 0;    
    dest_buffer_variables->dw1.v_ls_offset = 0;

    if ((picture_structure & XVMC_FRAME_PICTURE) == XVMC_FRAME_PICTURE) {
        ;
    } else if ((picture_structure & XVMC_FRAME_PICTURE) == XVMC_TOP_FIELD) {
        dest_buffer_variables->dw1.v_ls = 1;
    } else if ((picture_structure & XVMC_FRAME_PICTURE) == XVMC_BOTTOM_FIELD) {
        dest_buffer_variables->dw1.v_ls = 1;
        dest_buffer_variables->dw1.v_ls_offset = 1;
    }

    /* 3DSTATE_DEST_BUFFER_VARIABLES_MPEG */
    dest_buffer_variables_mpeg = (struct i915_3dstate_dest_buffer_variables_mpeg *)(++dest_buffer_variables);
    memset(dest_buffer_variables_mpeg, 0, sizeof(*dest_buffer_variables_mpeg));
    dest_buffer_variables_mpeg->dw0.type = CMD_3D;
    dest_buffer_variables_mpeg->dw0.opcode = OPC_3DSTATE_DEST_BUFFER_VARIABLES_MPEG;
    dest_buffer_variables_mpeg->dw0.length = 1;
    dest_buffer_variables_mpeg->dw1.decode_mode = MPEG_DECODE_MC;
    dest_buffer_variables_mpeg->dw1.rcontrol = 0;               /* for MPEG-1/MPEG-2 */
    dest_buffer_variables_mpeg->dw1.bidir_avrg_control = 0;     /* for MPEG-1/MPEG-2/MPEG-4 */ 
    dest_buffer_variables_mpeg->dw1.abort_on_error = 1;
    dest_buffer_variables_mpeg->dw1.intra8 = 0;         /* 16-bit formatted correction data */
    dest_buffer_variables_mpeg->dw1.tff = 1;            

    if (picture_structure & XVMC_FRAME_PICTURE) {
        ;
    } else if (picture_structure & XVMC_TOP_FIELD) {
        if (flags & XVMC_SECOND_FIELD)
            dest_buffer_variables_mpeg->dw1.tff = 0;
        else
            dest_buffer_variables_mpeg->dw1.tff = 1;
    } else if (picture_structure & XVMC_BOTTOM_FIELD) {
        if (flags & XVMC_SECOND_FIELD)
            dest_buffer_variables_mpeg->dw1.tff = 1;
        else
            dest_buffer_variables_mpeg->dw1.tff = 0;
    }
        
    dest_buffer_variables_mpeg->dw1.v_subsample_factor = MC_SUB_1V;
    dest_buffer_variables_mpeg->dw1.h_subsample_factor = MC_SUB_1H;
    dest_buffer_variables_mpeg->dw1.picture_width = (w >> 4);     /* in macroblocks */
    dest_buffer_variables_mpeg->dw2.picture_coding_type = picture_coding_type;

    /* 3DSATE_BUFFER_INFO */
    /* CORRECTION DATA */
    buffer_info = (struct i915_3dstate_buffer_info *)(++dest_buffer_variables_mpeg);
    memset(buffer_info, 0, sizeof(*buffer_info));
    buffer_info->dw0.type = CMD_3D;
    buffer_info->dw0.opcode = OPC_3DSTATE_BUFFER_INFO;
    buffer_info->dw0.length = 1;
    buffer_info->dw1.aux_id = 0;
    buffer_info->dw1.buffer_id = BUFFERID_MC_INTRA_CORR;
    buffer_info->dw1.aux_id = 0;
    buffer_info->dw1.fence_regs = 0; 
    buffer_info->dw1.tiled_surface = 0; 
    buffer_info->dw1.walk = 0;
    buffer_info->dw1.pitch = 0;
    buffer_info->dw2.base_address = (pI915XvMC->corrdata.offset >> 2);  /* starting DWORD address */
}

_STATIC_ void i915_mc_map_state_buffer(XvMCContext *context, 
                                       i915XvMCSurface *privTarget,
                                       i915XvMCSurface *privPast,
                                       i915XvMCSurface *privFuture)
{
    struct i915_3dstate_map_state *map_state;
    struct texture_map *tm;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    unsigned int w = context->width, h = context->height;
 
    /* 3DSATE_MAP_STATE: Y */
    map_state = (struct i915_3dstate_map_state *)pI915XvMC->msb.map;
    memset(map_state, 0, sizeof(*map_state));
    map_state->dw0.type = CMD_3D;
    map_state->dw0.opcode = OPC_3DSTATE_MAP_STATE;
    map_state->dw0.retain = 1;
    map_state->dw0.length = 6;
    map_state->dw1.map_mask = MAP_MAP0 | MAP_MAP1;

    /* texture map: Forward (Past) */
    tm = (struct texture_map *)(++map_state);
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (YOFFSET(privPast) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;        /* FIXME: tiled y for performace */
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;      /* 8bit */
    tm->tm1.surface_fmt = 1;    /* 8bit */
    tm->tm1.width = w - 1;
    tm->tm1.height = h - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privPast->yStride >> 2) - 1;       /* in DWords - 1 */

    /* texture map: Backward (Future) */
    ++tm;
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (YOFFSET(privFuture) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;      /* 8bit */
    tm->tm1.surface_fmt = 1;    /* 8bit */
    tm->tm1.width = w - 1;
    tm->tm1.height = h - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privFuture->yStride >> 2) - 1;

    /* 3DSATE_MAP_STATE: U */
    map_state = (struct i915_3dstate_map_state *)(++tm);
    memset(map_state, 0, sizeof(*map_state));
    map_state->dw0.type = CMD_3D;
    map_state->dw0.opcode = OPC_3DSTATE_MAP_STATE;
    map_state->dw0.retain = 1;
    map_state->dw0.length = 6;
    map_state->dw1.map_mask = MAP_MAP0 | MAP_MAP1;

    /* texture map: Forward */
    tm = (struct texture_map *)(++map_state);
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (UOFFSET(privPast) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;      /* 8bit */
    tm->tm1.surface_fmt = 1;    /* 8bit */
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privPast->uvStride >> 2) - 1;       /* in DWords - 1 */

    /* texture map: Backward */
    ++tm;
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (UOFFSET(privFuture) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privFuture->uvStride >> 2) - 1;     

    /* 3DSATE_MAP_STATE: V */
    map_state = (struct i915_3dstate_map_state *)(++tm);
    memset(map_state, 0, sizeof(*map_state));
    map_state->dw0.type = CMD_3D;
    map_state->dw0.opcode = OPC_3DSTATE_MAP_STATE;
    map_state->dw0.retain = 1;
    map_state->dw0.length = 6;
    map_state->dw1.map_mask = MAP_MAP0 | MAP_MAP1;

    /* texture map: Forward */
    tm = (struct texture_map *)(++map_state);
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (VOFFSET(privPast) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privPast->uvStride >> 2) - 1;       /* in DWords - 1 */

    /* texture map: Backward */
    ++tm;
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = (VOFFSET(privFuture) >> 2);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 0;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privFuture->uvStride >> 2) - 1;
}

_STATIC_ void i915_mc_load_sis_msb_buffers(XvMCContext *context)
{
    struct i915_3dstate_load_indirect *load_indirect;
    sis_state *sis = NULL;
    msb_state *msb = NULL;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    void *base = NULL;
    unsigned int size;
    int mem_select = 1;

    /* 3DSTATE_LOAD_INDIRECT */
    size = sizeof(*load_indirect) + sizeof(*sis) + sizeof(*msb);
    base = calloc(1, size);
    load_indirect = (struct i915_3dstate_load_indirect *)base;
    load_indirect->dw0.type = CMD_3D;
    load_indirect->dw0.opcode = OPC_3DSTATE_LOAD_INDIRECT;
    load_indirect->dw0.block_mask = BLOCK_SIS | BLOCK_MSB;
    load_indirect->dw0.length = (size >> 2) - 2;

    if (pI915XvMC->deviceID == PCI_CHIP_I915_G ||
        pI915XvMC->deviceID == PCI_CHIP_I915_GM ||
        pI915XvMC->deviceID == PCI_CHIP_I945_G ||
        pI915XvMC->deviceID == PCI_CHIP_I945_GM)
        mem_select = 0;

    load_indirect->dw0.mem_select = mem_select;

    /* SIS */
    sis = (sis_state *)(++load_indirect);
    sis->dw0.valid = 1;
    sis->dw0.force = 1;
    sis->dw1.length = 16; // 4 * 3 + 2 + 3 - 1

    if (mem_select)
        sis->dw0.buffer_address = (pI915XvMC->sis.offset >> 2);
    else
        sis->dw0.buffer_address = (pI915XvMC->sis.bus_addr >> 2);

    /* MSB */
    msb = (msb_state *)(++sis);
    msb->dw0.valid = 1;
    msb->dw0.force = 1;
    msb->dw1.length = 23; // 3 * 8 - 1

    if (mem_select)
        msb->dw0.buffer_address = (pI915XvMC->msb.offset >> 2);
    else
        msb->dw0.buffer_address = (pI915XvMC->msb.bus_addr >> 2);

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);
}

_STATIC_ void i915_mc_mpeg_set_origin(XvMCContext *context, XvMCMacroBlock *mb)
{
    struct i915_3dmpeg_set_origin set_origin;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;

    /* 3DMPEG_SET_ORIGIN */
    memset(&set_origin, 0, sizeof(set_origin));
    set_origin.dw0.type = CMD_3D;
    set_origin.dw0.opcode = OPC_3DMPEG_SET_ORIGIN;
    set_origin.dw0.length = 0;
    set_origin.dw1.h_origin = mb->x;
    set_origin.dw1.v_origin = mb->y;

    intelBatchbufferData(pI915XvMC, &set_origin, sizeof(set_origin), 0);
}

_STATIC_ void i915_mc_mpeg_macroblock_ipicture(XvMCContext *context, XvMCMacroBlock *mb)
{
    struct i915_3dmpeg_macroblock_ipicture macroblock_ipicture;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;

    /* 3DMPEG_MACROBLOCK_IPICTURE */
    memset(&macroblock_ipicture, 0, sizeof(macroblock_ipicture));
    macroblock_ipicture.dw0.type = CMD_3D;
    macroblock_ipicture.dw0.opcode = OPC_3DMPEG_MACROBLOCK_IPICTURE;
    macroblock_ipicture.dw0.dct_type = (mb->dct_type == XVMC_DCT_TYPE_FIELD);

    intelBatchbufferData(pI915XvMC, &macroblock_ipicture, sizeof(macroblock_ipicture), 0);
}


_STATIC_ void i915_mc_mpeg_macroblock_0mv(XvMCContext *context, XvMCMacroBlock *mb)
{
    struct i915_3dmpeg_macroblock_0mv macroblock_0mv;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;

    /* 3DMPEG_MACROBLOCK(0mv) */
    memset(&macroblock_0mv, 0, sizeof(macroblock_0mv));
    macroblock_0mv.header.dw0.type = CMD_3D;
    macroblock_0mv.header.dw0.opcode = OPC_3DMPEG_MACROBLOCK;
    macroblock_0mv.header.dw0.length = 0;
    macroblock_0mv.header.dw1.mb_intra = 1;     /* should be 1 */ 
    macroblock_0mv.header.dw1.forward = 0;      /* should be 0 */
    macroblock_0mv.header.dw1.backward = 0;     /* should be 0 */
    macroblock_0mv.header.dw1.h263_4mv = 0;     /* should be 0 */
    macroblock_0mv.header.dw1.dct_type = (mb->dct_type == XVMC_DCT_TYPE_FIELD);
    
/*
    if (!mb->coded_block_pattern)
        macroblock_0mv.header.dw1.dct_type = XVMC_DCT_TYPE_FRAME;
*/

    macroblock_0mv.header.dw1.motion_type = 0; // (mb->motion_type & 0x3);
    macroblock_0mv.header.dw1.vertical_field_select = 0; // mb->motion_vertical_field_select & 0xf;
    macroblock_0mv.header.dw1.coded_block_pattern = mb->coded_block_pattern;
    macroblock_0mv.header.dw1.skipped_macroblocks = 0;

    intelBatchbufferData(pI915XvMC, &macroblock_0mv, sizeof(macroblock_0mv), 0);
}

_STATIC_ void i915_mc_mpeg_macroblock_1fbmv(XvMCContext *context, XvMCMacroBlock *mb)
{
    struct i915_3dmpeg_macroblock_1fbmv macroblock_1fbmv;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    
    /* Motion Vectors */
    su_t fmv;
    su_t bmv;

    /* 3DMPEG_MACROBLOCK(1fbmv) */
    memset(&macroblock_1fbmv, 0, sizeof(macroblock_1fbmv));
    macroblock_1fbmv.header.dw0.type = CMD_3D;
    macroblock_1fbmv.header.dw0.opcode = OPC_3DMPEG_MACROBLOCK;
    macroblock_1fbmv.header.dw0.length = 2;
    macroblock_1fbmv.header.dw1.mb_intra = 0;   /* should be 0 */ 
    macroblock_1fbmv.header.dw1.forward = ((mb->macroblock_type & XVMC_MB_TYPE_MOTION_FORWARD) ? 1 : 0);
    macroblock_1fbmv.header.dw1.backward = ((mb->macroblock_type & XVMC_MB_TYPE_MOTION_BACKWARD) ? 1 : 0);
    macroblock_1fbmv.header.dw1.h263_4mv = 0;   /* should be 0 */
    macroblock_1fbmv.header.dw1.dct_type = (mb->dct_type == XVMC_DCT_TYPE_FIELD);
    
    if (!(mb->coded_block_pattern & 0x3f))
        macroblock_1fbmv.header.dw1.dct_type = XVMC_DCT_TYPE_FRAME;

    macroblock_1fbmv.header.dw1.motion_type = (mb->motion_type & 0x03);
    macroblock_1fbmv.header.dw1.vertical_field_select = (mb->motion_vertical_field_select & 0x0f);
    macroblock_1fbmv.header.dw1.coded_block_pattern = mb->coded_block_pattern; 
    macroblock_1fbmv.header.dw1.skipped_macroblocks = 0;      

    fmv.s[0] = mb->PMV[0][0][0];
    fmv.s[1] = mb->PMV[0][0][1];
    bmv.s[0] = mb->PMV[0][1][0];
    bmv.s[1] = mb->PMV[0][1][1];

    macroblock_1fbmv.dw2 = fmv.u[0];
    macroblock_1fbmv.dw3 = bmv.u[0];
    
    intelBatchbufferData(pI915XvMC, &macroblock_1fbmv, sizeof(macroblock_1fbmv), 0);
}

_STATIC_ void i915_mc_mpeg_macroblock_2fbmv(XvMCContext *context, XvMCMacroBlock *mb, unsigned int ps)
{
    struct i915_3dmpeg_macroblock_2fbmv macroblock_2fbmv;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    
    /* Motion Vectors */
    su_t fmv;
    su_t bmv;

    /* 3DMPEG_MACROBLOCK(2fbmv) */
    memset(&macroblock_2fbmv, 0, sizeof(macroblock_2fbmv));
    macroblock_2fbmv.header.dw0.type = CMD_3D;
    macroblock_2fbmv.header.dw0.opcode = OPC_3DMPEG_MACROBLOCK;
    macroblock_2fbmv.header.dw0.length = 4;
    macroblock_2fbmv.header.dw1.mb_intra = 0;   /* should be 0 */
    macroblock_2fbmv.header.dw1.forward = ((mb->macroblock_type & XVMC_MB_TYPE_MOTION_FORWARD) ? 1 : 0);
    macroblock_2fbmv.header.dw1.backward = ((mb->macroblock_type & XVMC_MB_TYPE_MOTION_BACKWARD) ? 1 : 0);
    macroblock_2fbmv.header.dw1.h263_4mv = 0;   /* should be 0 */
    macroblock_2fbmv.header.dw1.dct_type = (mb->dct_type == XVMC_DCT_TYPE_FIELD);
    
    if (!(mb->coded_block_pattern & 0x3f))
        macroblock_2fbmv.header.dw1.dct_type = XVMC_DCT_TYPE_FRAME;

    macroblock_2fbmv.header.dw1.motion_type = (mb->motion_type & 0x03);
    macroblock_2fbmv.header.dw1.vertical_field_select = (mb->motion_vertical_field_select & 0x0f);
    macroblock_2fbmv.header.dw1.coded_block_pattern = mb->coded_block_pattern;
    macroblock_2fbmv.header.dw1.skipped_macroblocks = 0;

    fmv.s[0] = mb->PMV[0][0][0];
    fmv.s[1] = mb->PMV[0][0][1];
    fmv.s[2] = mb->PMV[1][0][0];
    fmv.s[3] = mb->PMV[1][0][1];
    bmv.s[0] = mb->PMV[0][1][0];
    bmv.s[1] = mb->PMV[0][1][1];
    bmv.s[2] = mb->PMV[1][1][0];
    bmv.s[3] = mb->PMV[1][1][1];

    if ((ps & XVMC_FRAME_PICTURE) == XVMC_FRAME_PICTURE) {
        if ((mb->motion_type & 3) == XVMC_PREDICTION_FIELD) {
            fmv.s[0] = mb->PMV[0][0][0];
            fmv.s[1] = mb->PMV[0][0][1] >> 1;
            fmv.s[2] = mb->PMV[1][0][0];
            fmv.s[3] = mb->PMV[1][0][1] >> 1;
            bmv.s[0] = mb->PMV[0][1][0];
            bmv.s[1] = mb->PMV[0][1][1] >> 1;
            bmv.s[2] = mb->PMV[1][1][0];
            bmv.s[3] = mb->PMV[1][1][1] >> 1;
        } else if ((mb->motion_type & 3) == XVMC_PREDICTION_DUAL_PRIME) {
            fmv.s[0] = mb->PMV[0][0][0];
            fmv.s[1] = mb->PMV[0][0][1] >> 1;
            fmv.s[2] = mb->PMV[0][0][0];
            fmv.s[3] = mb->PMV[0][0][1] >> 1;  // MPEG2 MV[0][1] isn't used
            bmv.s[0] = mb->PMV[1][0][0];
            bmv.s[1] = mb->PMV[1][0][1] >> 1;
            bmv.s[2] = mb->PMV[1][1][0];
            bmv.s[3] = mb->PMV[1][1][1] >> 1;
        }
    }

    macroblock_2fbmv.dw2 = fmv.u[0];
    macroblock_2fbmv.dw3 = bmv.u[0];
    macroblock_2fbmv.dw4 = fmv.u[1];
    macroblock_2fbmv.dw5 = bmv.u[1];

    intelBatchbufferData(pI915XvMC, &macroblock_2fbmv, sizeof(macroblock_2fbmv), 0);
}

/* for MC context initialization */
_STATIC_ void i915_mc_sampler_state_buffer(XvMCContext *context)
{
    struct i915_3dstate_sampler_state *sampler_state;
    struct texture_sampler *ts;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    
    /* 3DSATE_SAMPLER_STATE */
    sampler_state = (struct i915_3dstate_sampler_state *)pI915XvMC->ssb.map;
    memset(sampler_state, 0, sizeof(*sampler_state));
    sampler_state->dw0.type = CMD_3D;
    sampler_state->dw0.opcode = OPC_3DSTATE_SAMPLER_STATE;
    sampler_state->dw0.length = 6;
    sampler_state->dw1.sampler_masker = SAMPLER_SAMPLER0 | SAMPLER_SAMPLER1;

    /* Sampler 0 */
    ts = (struct texture_sampler *)(++sampler_state);
    memset(ts, 0, sizeof(*ts));
    ts->ts0.reverse_gamma = 0;
    ts->ts0.planar2packet = 0;
    ts->ts0.color_conversion = 0;
    ts->ts0.chromakey_index = 0;
    ts->ts0.base_level = 0;
    ts->ts0.mip_filter = MIPFILTER_NONE;        /* NONE */
    ts->ts0.mag_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.min_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.lod_bias = 0;       /* 0.0 */
    ts->ts0.shadow_enable = 0;
    ts->ts0.max_anisotropy = ANISORATIO_2;
    ts->ts0.shadow_function = PREFILTEROP_ALWAYS;
    ts->ts1.min_lod = 0;        /* 0.0 Maximum Mip Level */
    ts->ts1.kill_pixel = 0;
    ts->ts1.keyed_texture_filter = 0;
    ts->ts1.chromakey_enable = 0;
    ts->ts1.tcx_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcy_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcz_control = TEXCOORDMODE_CLAMP;
    ts->ts1.normalized_coor = 0;
    ts->ts1.map_index = 0;
    ts->ts1.east_deinterlacer = 0;
    ts->ts2.default_color = 0;

    /* Sampler 1 */
    ++ts;
    memset(ts, 0, sizeof(*ts));
    ts->ts0.reverse_gamma = 0;
    ts->ts0.planar2packet = 0;
    ts->ts0.color_conversion = 0;
    ts->ts0.chromakey_index = 0;
    ts->ts0.base_level = 0;
    ts->ts0.mip_filter = MIPFILTER_NONE;        /* NONE */
    ts->ts0.mag_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.min_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.lod_bias = 0;       /* 0.0 */
    ts->ts0.shadow_enable = 0;
    ts->ts0.max_anisotropy = ANISORATIO_2;
    ts->ts0.shadow_function = PREFILTEROP_ALWAYS;
    ts->ts1.min_lod = 0;        /* 0.0 Maximum Mip Level */
    ts->ts1.kill_pixel = 0;
    ts->ts1.keyed_texture_filter = 0;
    ts->ts1.chromakey_enable = 0;
    ts->ts1.tcx_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcy_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcz_control = TEXCOORDMODE_CLAMP;
    ts->ts1.normalized_coor = 0;
    ts->ts1.map_index = 1;
    ts->ts1.east_deinterlacer = 0;
    ts->ts2.default_color = 0;
}

_STATIC_ void i915_inst_arith(unsigned int *inst,
                            unsigned int op,
                            unsigned int dest,
                            unsigned int mask,
                            unsigned int saturate,
                            unsigned int src0, unsigned int src1, unsigned int src2)
{
    dest = UREG(GET_UREG_TYPE(dest), GET_UREG_NR(dest));
    *inst = (op | A0_DEST(dest) | mask | saturate | A0_SRC0(src0));
    inst++;
    *inst = (A1_SRC0(src0) | A1_SRC1(src1));
    inst++;
    *inst = (A2_SRC1(src1) | A2_SRC2(src2));
}

_STATIC_ void i915_inst_decl(unsigned int *inst, 
                           unsigned int type,
                           unsigned int nr,
                           unsigned int d0_flags)
{
    unsigned int reg = UREG(type, nr);
    
    *inst = (D0_DCL | D0_DEST(reg) | d0_flags);
    inst++;
    *inst = D1_MBZ;
    inst++;
    *inst = D2_MBZ;
}

_STATIC_ void i915_inst_texld(unsigned int *inst,
                              unsigned int op,
                              unsigned int dest,
                              unsigned int coord,
                              unsigned int sampler)
{
   dest = UREG(GET_UREG_TYPE(dest), GET_UREG_NR(dest));
   *inst = (op | T0_DEST(dest) | T0_SAMPLER(sampler));
   inst++;
   *inst = T1_ADDRESS_REG(coord);
   inst++;
   *inst = T2_MBZ;
}

_STATIC_ void i915_mc_pixel_shader_program_buffer(XvMCContext *context)
{
    struct i915_3dstate_pixel_shader_program *pixel_shader_program;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    unsigned int *inst;
    unsigned int dest, src0, src1, src2;

    /* Shader 0 */
    pixel_shader_program = (struct i915_3dstate_pixel_shader_program *)pI915XvMC->psp.map;
    memset(pixel_shader_program, 0, sizeof(*pixel_shader_program));
    pixel_shader_program->dw0.type = CMD_3D;
    pixel_shader_program->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_PROGRAM;
    pixel_shader_program->dw0.retain = 1;
    pixel_shader_program->dw0.length = 2;
    /* mov oC, c0.0000 */
    inst = (unsigned int*)(++pixel_shader_program);
    dest = UREG(REG_TYPE_OC, 0);
    src0 = UREG(REG_TYPE_CONST, 0);
    src1 = 0;
    src2 = 0;
    i915_inst_arith(inst, A0_MOV, dest, A0_DEST_CHANNEL_ALL,
                    A0_DEST_SATURATE, src0, src1, src2);
    inst += 3;

    /* Shader 1 */
    pixel_shader_program = (struct i915_3dstate_pixel_shader_program *)inst;
    memset(pixel_shader_program, 0, sizeof(*pixel_shader_program));
    pixel_shader_program->dw0.type = CMD_3D;
    pixel_shader_program->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_PROGRAM;
    pixel_shader_program->dw0.retain = 1;
    pixel_shader_program->dw0.length = 14;
    /* dcl t0.xy */
    inst = (unsigned int*)(++pixel_shader_program);
    i915_inst_decl(inst, REG_TYPE_T, T_TEX0, D0_CHANNEL_XY);
    /* dcl t1.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX1, D0_CHANNEL_XY);
    /* dcl_2D s0 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 0, D0_SAMPLE_TYPE_2D);
    /* texld r0, t0, s0 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0); 
    src0 = UREG(REG_TYPE_T, 0); /* COORD */
    src1 = UREG(REG_TYPE_S, 0); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* mov oC, r0 */
    inst += 3;
    dest = UREG(REG_TYPE_OC, 0);
    src0 = UREG(REG_TYPE_R, 0);
    src1 = src2 = 0;
    i915_inst_arith(inst, A0_MOV, dest, A0_DEST_CHANNEL_ALL,
                    A0_DEST_SATURATE, src0, src1, src2);
    inst += 3;

    /* Shader 2 */
    pixel_shader_program = (struct i915_3dstate_pixel_shader_program *)inst;
    memset(pixel_shader_program, 0, sizeof(*pixel_shader_program));
    pixel_shader_program->dw0.type = CMD_3D;
    pixel_shader_program->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_PROGRAM;
    pixel_shader_program->dw0.retain = 1;
    pixel_shader_program->dw0.length = 14;
    /* dcl t2.xy */
    inst = (unsigned int*)(++pixel_shader_program);
    i915_inst_decl(inst, REG_TYPE_T, T_TEX2, D0_CHANNEL_XY);
    /* dcl t3.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX3, D0_CHANNEL_XY);
    /* dcl_2D s1 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 1, D0_SAMPLE_TYPE_2D);
    /* texld r0, t2, s1 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0); 
    src0 = UREG(REG_TYPE_T, 2); /* COORD */
    src1 = UREG(REG_TYPE_S, 1); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* mov oC, r0 */
    inst += 3;
    dest = UREG(REG_TYPE_OC, 0);
    src0 = UREG(REG_TYPE_R, 0);
    src1 = src2 = 0;
    i915_inst_arith(inst, A0_MOV, dest, A0_DEST_CHANNEL_ALL,
                    A0_DEST_SATURATE, src0, src1, src2);
    inst += 3;

    /* Shader 3 */
    pixel_shader_program = (struct i915_3dstate_pixel_shader_program *)inst;
    memset(pixel_shader_program, 0, sizeof(*pixel_shader_program));
    pixel_shader_program->dw0.type = CMD_3D;
    pixel_shader_program->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_PROGRAM;
    pixel_shader_program->dw0.retain = 1;
    pixel_shader_program->dw0.length = 29;
    /* dcl t0.xy */
    inst = (unsigned int*)(++pixel_shader_program);
    i915_inst_decl(inst, REG_TYPE_T, T_TEX0, D0_CHANNEL_XY);
    /* dcl t1.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX1, D0_CHANNEL_XY);
    /* dcl t2.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX2, D0_CHANNEL_XY);
    /* dcl t3.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX3, D0_CHANNEL_XY);
    /* dcl_2D s0 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 0, D0_SAMPLE_TYPE_2D);
    /* dcl_2D s1 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 1, D0_SAMPLE_TYPE_2D);
    /* texld r0, t0, s0 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0); 
    src0 = UREG(REG_TYPE_T, 0); /* COORD */
    src1 = UREG(REG_TYPE_S, 0); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* texld r1, t2, s1 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 1); 
    src0 = UREG(REG_TYPE_T, 2); /* COORD */
    src1 = UREG(REG_TYPE_S, 1); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* add r0, r0, r1 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0);
    src0 = UREG(REG_TYPE_R, 0);
    src1 = UREG(REG_TYPE_R, 1);
    src2 = 0;
    i915_inst_arith(inst, A0_ADD, dest, A0_DEST_CHANNEL_ALL,
                    0 /* A0_DEST_SATURATE */, src0, src1, src2);
    /* mul oC, r0, c0 */
    inst += 3;
    dest = UREG(REG_TYPE_OC, 0);
    src0 = UREG(REG_TYPE_R, 0);
    src1 = UREG(REG_TYPE_CONST, 0);
    src2 = 0;
    i915_inst_arith(inst, A0_MUL, dest, A0_DEST_CHANNEL_ALL,
                    A0_DEST_SATURATE, src0, src1, src2);
    inst += 3;
}

_STATIC_ void i915_mc_pixel_shader_constants_buffer(XvMCContext *context)
{
    struct i915_3dstate_pixel_shader_constants *pixel_shader_constants;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    float *value;

    pixel_shader_constants = (struct i915_3dstate_pixel_shader_constants *)pI915XvMC->psc.map;
    memset(pixel_shader_constants, 0, sizeof(*pixel_shader_constants));
    pixel_shader_constants->dw0.type = CMD_3D;
    pixel_shader_constants->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_CONSTANTS;
    pixel_shader_constants->dw0.length = 4;
    pixel_shader_constants->dw1.reg_mask = REG_CR0;
    value = (float *)(++pixel_shader_constants);
    *(value++) = 0.5;
    *(value++) = 0.5;
    *(value++) = 0.5;
    *(value++) = 0.5;
}

_STATIC_ void i915_mc_one_time_state_initialization(XvMCContext *context)
{
    struct i915_3dstate_load_state_immediate_1 *load_state_immediate_1 = NULL;
    struct s3_dword *s3 = NULL;
    struct s6_dword *s6 = NULL;
    struct i915_3dstate_load_indirect *load_indirect = NULL;
    dis_state *dis = NULL;
    ssb_state *ssb = NULL;
    psp_state *psp = NULL;
    psc_state *psc = NULL;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    unsigned int size;
    void *base = NULL;
    int mem_select = 1;

    /* 3DSTATE_LOAD_STATE_IMMEDIATE_1 */
    size = sizeof(*load_state_immediate_1) + sizeof(*s3) + sizeof(*s6);
    base = calloc(1, size);
    load_state_immediate_1 = (struct i915_3dstate_load_state_immediate_1 *)base;
    load_state_immediate_1->dw0.type = CMD_3D;
    load_state_immediate_1->dw0.opcode = OPC_3DSTATE_LOAD_STATE_IMMEDIATE_1;
    load_state_immediate_1->dw0.load_s3 = 1;
    load_state_immediate_1->dw0.load_s6 = 1;
    load_state_immediate_1->dw0.length = (size >> 2) - 2;

    s3 = (struct s3_dword *)(++load_state_immediate_1);
    s3->set0_pcd = 1;
    s3->set1_pcd = 1;
    s3->set2_pcd = 1;
    s3->set3_pcd = 1;
    s3->set4_pcd = 1;
    s3->set5_pcd = 1;
    s3->set6_pcd = 1;
    s3->set7_pcd = 1;

    s6 = (struct s6_dword *)(++s3);
    s6->alpha_test_enable = 0;
    s6->alpha_test_function = 0;
    s6->alpha_reference_value = 0;
    s6->depth_test_enable = 1;
    s6->depth_test_function = 0;
    s6->color_buffer_blend = 0;
    s6->color_blend_function = 0;
    s6->src_blend_factor = 1;
    s6->dest_blend_factor = 1;
    s6->depth_buffer_write = 0;
    s6->color_buffer_write = 1;
    s6->triangle_pv = 0;

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);

    /* 3DSTATE_LOAD_INDIRECT */
    size = sizeof(*load_indirect) + sizeof(*dis) + sizeof(*ssb) + sizeof(*psp) + sizeof(*psc);
    base = calloc(1, size);
    load_indirect = (struct i915_3dstate_load_indirect *)base;
    load_indirect->dw0.type = CMD_3D;
    load_indirect->dw0.opcode = OPC_3DSTATE_LOAD_INDIRECT;
    load_indirect->dw0.block_mask = BLOCK_DIS | BLOCK_SSB | BLOCK_PSP | BLOCK_PSC;
    load_indirect->dw0.length = (size >> 2) - 2;

    if (pI915XvMC->deviceID == PCI_CHIP_I915_G ||
        pI915XvMC->deviceID == PCI_CHIP_I915_GM ||
        pI915XvMC->deviceID == PCI_CHIP_I945_G ||
        pI915XvMC->deviceID == PCI_CHIP_I945_GM)
        mem_select = 0;

    load_indirect->dw0.mem_select = mem_select;

    /* DIS */
    dis = (dis_state *)(++load_indirect);
    dis->dw0.valid = 0;
    dis->dw0.reset = 0;
    dis->dw0.buffer_address = 0;

    /* SSB */
    ssb = (ssb_state *)(++dis);
    ssb->dw0.valid = 1;
    ssb->dw0.force = 1;
    ssb->dw1.length = 7; /* 8 - 1 */

    if (mem_select)
        ssb->dw0.buffer_address = (pI915XvMC->ssb.offset >> 2);
    else
        ssb->dw0.buffer_address = (pI915XvMC->ssb.bus_addr >> 2);

    /* PSP */
    psp = (psp_state *)(++ssb);
    psp->dw0.valid = 1;
    psp->dw0.force = 1;
    psp->dw1.length = 66; /* 4 + 16 + 16 + 31 - 1 */
    
    if (mem_select)
        psp->dw0.buffer_address = (pI915XvMC->psp.offset >> 2);
    else
        psp->dw0.buffer_address = (pI915XvMC->psp.bus_addr >> 2);

    /* PSC */
    psc = (psc_state *)(++psp);
    psc->dw0.valid = 1;
    psc->dw0.force = 1;
    psc->dw1.length = 5; /* 6 - 1 */

    if (mem_select)
        psc->dw0.buffer_address = (pI915XvMC->psc.offset >> 2);
    else
        psc->dw0.buffer_address = (pI915XvMC->psc.bus_addr >> 2);

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);
}

_STATIC_ void i915_mc_invalidate_subcontext_buffers(XvMCContext *context, unsigned int mask)
{
    struct i915_3dstate_load_indirect *load_indirect = NULL;
    sis_state *sis = NULL;
    dis_state *dis = NULL;
    ssb_state *ssb = NULL;
    msb_state *msb = NULL;
    psp_state *psp = NULL;
    psc_state *psc = NULL;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)context->privData;
    unsigned int size;
    void *base = NULL, *ptr = NULL;

    size = sizeof(*load_indirect);
    if (mask & BLOCK_SIS)
        size += sizeof(*sis);
    if (mask & BLOCK_DIS)
        size += sizeof(*dis);
    if (mask & BLOCK_SSB)
        size += sizeof(*ssb);
    if (mask & BLOCK_MSB)
        size += sizeof(*msb);
    if (mask & BLOCK_PSP)
        size += sizeof(*psp);
    if (mask & BLOCK_PSC)
        size += sizeof(*psc);

    if (size == sizeof(*load_indirect)) {
        XVMC_ERR("There must be at least one bit set\n");
        return;
    }

    /* 3DSTATE_LOAD_INDIRECT */
    base = calloc(1, size);
    load_indirect = (struct i915_3dstate_load_indirect *)base;
    load_indirect->dw0.type = CMD_3D;
    load_indirect->dw0.opcode = OPC_3DSTATE_LOAD_INDIRECT;

    if (pI915XvMC->deviceID == PCI_CHIP_I915_G ||
        pI915XvMC->deviceID == PCI_CHIP_I915_GM ||
        pI915XvMC->deviceID == PCI_CHIP_I945_G ||
        pI915XvMC->deviceID == PCI_CHIP_I945_GM)
        load_indirect->dw0.mem_select = 0;
    else
        load_indirect->dw0.mem_select = 1;

    load_indirect->dw0.block_mask = mask;
    load_indirect->dw0.length = (size >> 2) - 2;
    ptr = ++load_indirect;

    /* SIS */
    if (mask & BLOCK_SIS) {
        sis = (sis_state *)ptr;
        sis->dw0.valid = 0;
        sis->dw0.buffer_address = 0;
        sis->dw1.length = 0;
        ptr = ++sis;
    }

    /* DIS */
    if (mask & BLOCK_DIS) {
        dis = (dis_state *)ptr;
        dis->dw0.valid = 0;
        dis->dw0.reset = 0;
        dis->dw0.buffer_address = 0;
        ptr = ++dis;
    }

    /* SSB */
    if (mask & BLOCK_SSB) {
        ssb = (ssb_state *)ptr;
        ssb->dw0.valid = 0;
        ssb->dw0.buffer_address = 0;
        ssb->dw1.length = 0;
        ptr = ++ssb;
    }

    /* MSB */
    if (mask & BLOCK_MSB) {
        msb = (msb_state *)ptr;
        msb->dw0.valid = 0;
        msb->dw0.buffer_address = 0;
        msb->dw1.length = 0;
        ptr = ++msb;
    }

    /* PSP */
    if (mask & BLOCK_PSP) {
        psp = (psp_state *)ptr;
        psp->dw0.valid = 0;
        psp->dw0.buffer_address = 0;
        psp->dw1.length = 0;
        ptr = ++psp;
    }

    /* PSC */
    if (mask & BLOCK_PSC) {
        psc = (psc_state *)ptr;
        psc->dw0.valid = 0;
        psc->dw0.buffer_address = 0;
        psc->dw1.length = 0;
        ptr = ++psc;
    }

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);
}

_STATIC_ int i915_xvmc_map_buffers(i915XvMCContext *pI915XvMC)
{
    if (drmMap(pI915XvMC->fd,
               pI915XvMC->sis.handle,
               pI915XvMC->sis.size,
               (drmAddress *)&pI915XvMC->sis.map) != 0) {
        return -1;
    }

    if (drmMap(pI915XvMC->fd,
               pI915XvMC->ssb.handle,
               pI915XvMC->ssb.size,
               (drmAddress *)&pI915XvMC->ssb.map) != 0) {
        return -1;
    }

    if (drmMap(pI915XvMC->fd,
               pI915XvMC->msb.handle,
               pI915XvMC->msb.size,
               (drmAddress *)&pI915XvMC->msb.map) != 0) {
        return -1;
    }

    if (drmMap(pI915XvMC->fd,
               pI915XvMC->psp.handle,
               pI915XvMC->psp.size,
               (drmAddress *)&pI915XvMC->psp.map) != 0) {
        return -1;
    }

    if (drmMap(pI915XvMC->fd,
               pI915XvMC->psc.handle,
               pI915XvMC->psc.size,
               (drmAddress *)&pI915XvMC->psc.map) != 0) {
        return -1;
    }

    if (drmMap(pI915XvMC->fd,
               pI915XvMC->corrdata.handle,
               pI915XvMC->corrdata.size,
               (drmAddress *)&pI915XvMC->corrdata.map) != 0) {
        return -1;
    }
    
    if (drmMap(pI915XvMC->fd,
               pI915XvMC->batchbuffer.handle,
               pI915XvMC->batchbuffer.size,
               (drmAddress *)&pI915XvMC->batchbuffer.map) != 0) {
        return -1;
    }

    return 0;
}

_STATIC_ void i915_xvmc_unmap_buffers(i915XvMCContext *pI915XvMC)
{
    if (pI915XvMC->sis.map) {
        drmUnmap(pI915XvMC->sis.map, pI915XvMC->sis.size);
        pI915XvMC->sis.map = NULL;
    }

    if (pI915XvMC->ssb.map) {
        drmUnmap(pI915XvMC->ssb.map, pI915XvMC->ssb.size);
        pI915XvMC->ssb.map = NULL;
    }

    if (pI915XvMC->msb.map) {
        drmUnmap(pI915XvMC->msb.map, pI915XvMC->msb.size);
        pI915XvMC->msb.map = NULL;
    }

    if (pI915XvMC->psp.map) {
        drmUnmap(pI915XvMC->psp.map, pI915XvMC->psp.size);
        pI915XvMC->psp.map = NULL;
    }

    if (pI915XvMC->psc.map) {
        drmUnmap(pI915XvMC->psc.map, pI915XvMC->psc.size);
        pI915XvMC->psc.map = NULL;
    }

    if (pI915XvMC->corrdata.map) {
        drmUnmap(pI915XvMC->corrdata.map, pI915XvMC->corrdata.size);
        pI915XvMC->corrdata.map = NULL;
    }

    if (pI915XvMC->batchbuffer.map) {
        drmUnmap(pI915XvMC->batchbuffer.map, pI915XvMC->batchbuffer.size);
        pI915XvMC->batchbuffer.map = NULL;
    }
}

/*
 * Video post processing 
 */
_STATIC_ void i915_yuv2rgb_map_state_buffer(XvMCSurface *target_surface)
{
    struct i915_3dstate_map_state *map_state;
    struct texture_map *tm;
    i915XvMCSurface *privTarget = NULL;
    i915XvMCContext *pI915XvMC = NULL;
    unsigned int w = target_surface->width, h = target_surface->height;

    privTarget = (i915XvMCSurface *)target_surface->privData;
    pI915XvMC = (i915XvMCContext *)privTarget->privContext;
    /* 3DSATE_MAP_STATE */
    map_state = (struct i915_3dstate_map_state *)pI915XvMC->msb.map;
    memset(map_state, 0, sizeof(*map_state));
    map_state->dw0.type = CMD_3D;
    map_state->dw0.opcode = OPC_3DSTATE_MAP_STATE;
    map_state->dw0.retain = 0;
    map_state->dw0.length = 9;
    map_state->dw1.map_mask = MAP_MAP0 | MAP_MAP1 | MAP_MAP2;

    /* texture map 0: V Plane */
    tm = (struct texture_map *)(++map_state);
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = VOFFSET(privTarget);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 1;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privTarget->uvStride >> 2) - 1;    /* in DWords - 1 */

    /* texture map 1: Y Plane */
    ++tm;
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = YOFFSET(privTarget);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 1;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = w - 1;
    tm->tm1.height = h - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privTarget->yStride >> 2) - 1;     /* in DWords - 1 */

    /* texture map 2: U Plane */
    ++tm;
    memset(tm, 0, sizeof(*tm));
    tm->tm0.v_ls_offset = 0;
    tm->tm0.v_ls = 0;
    tm->tm0.base_address = UOFFSET(privTarget);
    tm->tm1.tile_walk = TILEWALK_XMAJOR;
    tm->tm1.tiled_surface = 0;
    tm->tm1.utilize_fence_regs = 1;
    tm->tm1.texel_fmt = 0;
    tm->tm1.surface_fmt = 1;
    tm->tm1.width = (w >> 1) - 1;
    tm->tm1.height = (h >> 1) - 1;
    tm->tm2.depth = 0;
    tm->tm2.max_lod = 0;
    tm->tm2.cube_face = 0;
    tm->tm2.pitch = (privTarget->uvStride >> 2) - 1;    /* in DWords - 1 */
}

_STATIC_ void i915_yuv2rgb_sampler_state_buffer(XvMCSurface *surface)
{
    struct i915_3dstate_sampler_state *sampler_state;
    struct texture_sampler *ts;
    i915XvMCSurface *privSurface = (i915XvMCSurface *)surface->privData;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)privSurface->privContext;
    
    /* 3DSATE_SAMPLER_STATE */
    sampler_state = (struct i915_3dstate_sampler_state *)pI915XvMC->ssb.map;
    memset(sampler_state, 0, sizeof(*sampler_state));
    sampler_state->dw0.type = CMD_3D;
    sampler_state->dw0.opcode = OPC_3DSTATE_SAMPLER_STATE;
    sampler_state->dw0.length = 9;
    sampler_state->dw1.sampler_masker = SAMPLER_SAMPLER0 | SAMPLER_SAMPLER1 | SAMPLER_SAMPLER2;

    /* Sampler 0 */
    ts = (struct texture_sampler *)(++sampler_state);
    memset(ts, 0, sizeof(*ts));
    ts->ts0.reverse_gamma = 0;
    ts->ts0.planar2packet = 1;
    ts->ts0.color_conversion = 1;
    ts->ts0.chromakey_index = 0;
    ts->ts0.base_level = 0;
    ts->ts0.mip_filter = MIPFILTER_NONE;        /* NONE */
    ts->ts0.mag_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.min_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.lod_bias = 0;
    ts->ts0.shadow_enable = 0;
    ts->ts0.max_anisotropy = ANISORATIO_2;
    ts->ts0.shadow_function = PREFILTEROP_ALWAYS;
    ts->ts1.min_lod = 0;        /* Maximum Mip Level */
    ts->ts1.kill_pixel = 0;
    ts->ts1.keyed_texture_filter = 0;
    ts->ts1.chromakey_enable = 0;
    ts->ts1.tcx_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcy_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcz_control = TEXCOORDMODE_CLAMP;
    ts->ts1.normalized_coor = 0;
    ts->ts1.map_index = 0;
    ts->ts1.east_deinterlacer = 0;
    ts->ts2.default_color = 0;

    /* Sampler 1 */
    ++ts;
    memset(ts, 0, sizeof(*ts));
    ts->ts0.reverse_gamma = 0;
    ts->ts0.planar2packet = 1;
    ts->ts0.color_conversion = 1;
    ts->ts0.chromakey_index = 0;
    ts->ts0.base_level = 0;
    ts->ts0.mip_filter = MIPFILTER_NONE;        /* NONE */
    ts->ts0.mag_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.min_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.lod_bias = 0;
    ts->ts0.shadow_enable = 0;
    ts->ts0.max_anisotropy = ANISORATIO_2;
    ts->ts0.shadow_function = PREFILTEROP_ALWAYS;
    ts->ts1.min_lod = 0;        /* Maximum Mip Level */
    ts->ts1.kill_pixel = 0;
    ts->ts1.keyed_texture_filter = 0;
    ts->ts1.chromakey_enable = 0;
    ts->ts1.tcx_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcy_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcz_control = TEXCOORDMODE_CLAMP;
    ts->ts1.normalized_coor = 0;
    ts->ts1.map_index = 1;
    ts->ts1.east_deinterlacer = 0;
    ts->ts2.default_color = 0;

    /* Sampler 2 */
    ++ts;
    memset(ts, 0, sizeof(*ts));
    ts->ts0.reverse_gamma = 0;
    ts->ts0.planar2packet = 1;
    ts->ts0.color_conversion = 1;
    ts->ts0.chromakey_index = 0;
    ts->ts0.base_level = 0;
    ts->ts0.mip_filter = MIPFILTER_NONE;        /* NONE */
    ts->ts0.mag_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.min_filter = MAPFILTER_LINEAR;      /* LINEAR */
    ts->ts0.lod_bias = 0;
    ts->ts0.shadow_enable = 0;
    ts->ts0.max_anisotropy = ANISORATIO_2;
    ts->ts0.shadow_function = PREFILTEROP_ALWAYS;
    ts->ts1.min_lod = 0;        /* Maximum Mip Level */
    ts->ts1.kill_pixel = 0;
    ts->ts1.keyed_texture_filter = 0;
    ts->ts1.chromakey_enable = 0;
    ts->ts1.tcx_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcy_control = TEXCOORDMODE_CLAMP;
    ts->ts1.tcz_control = TEXCOORDMODE_CLAMP;
    ts->ts1.normalized_coor = 0;
    ts->ts1.map_index = 2;
    ts->ts1.east_deinterlacer = 0;
    ts->ts2.default_color = 0;
}

_STATIC_ void i915_yuv2rgb_static_indirect_state_buffer(XvMCSurface *surface,
                                                      unsigned int dstaddr, 
                                                      int dstpitch)
{
    struct i915_3dstate_buffer_info *buffer_info;
    struct i915_3dstate_dest_buffer_variables *dest_buffer_variables;
    i915XvMCSurface *privSurface = (i915XvMCSurface *)surface->privData;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)privSurface->privContext;

    /* 3DSTATE_BUFFER_INFO */
    buffer_info = (struct i915_3dstate_buffer_info *)pI915XvMC->sis.map;
    memset(buffer_info, 0, sizeof(*buffer_info));
    buffer_info->dw0.type = CMD_3D;
    buffer_info->dw0.opcode = OPC_3DSTATE_BUFFER_INFO;
    buffer_info->dw0.length = 1;
    buffer_info->dw1.aux_id = 0;
    buffer_info->dw1.buffer_id = BUFFERID_COLOR_BACK;
    buffer_info->dw1.fence_regs = 1;   
    buffer_info->dw1.tiled_surface = 0;   /* linear */
    buffer_info->dw1.walk = TILEWALK_XMAJOR;
    buffer_info->dw1.pitch = dstpitch;
    buffer_info->dw2.base_address = dstaddr;

    /* 3DSTATE_DEST_BUFFER_VARIABLES */
    dest_buffer_variables = (struct i915_3dstate_dest_buffer_variables *)(++buffer_info);
    memset(dest_buffer_variables, 0, sizeof(*dest_buffer_variables));
    dest_buffer_variables->dw0.type = CMD_3D;
    dest_buffer_variables->dw0.opcode = OPC_3DSTATE_DEST_BUFFER_VARIABLES;
    dest_buffer_variables->dw0.length = 0;
    dest_buffer_variables->dw1.dest_v_bias = 8; /* FIXME 0x1000 .5 ??? */
    dest_buffer_variables->dw1.dest_h_bias = 8;
    dest_buffer_variables->dw1.color_fmt = COLORBUFFER_A8R8G8B8;  /* FIXME */
}

_STATIC_ void i915_yuv2rgb_pixel_shader_program_buffer(XvMCSurface *surface)
{
    struct i915_3dstate_pixel_shader_program *pixel_shader_program;
    i915XvMCSurface *privSurface = (i915XvMCSurface *)surface->privData;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)privSurface->privContext;
    unsigned int *inst;
    unsigned int dest, src0, src1;

    /* Shader 0 */
    pixel_shader_program = (struct i915_3dstate_pixel_shader_program *)pI915XvMC->psp.map;
    memset(pixel_shader_program, 0, sizeof(*pixel_shader_program));
    pixel_shader_program->dw0.type = CMD_3D;
    pixel_shader_program->dw0.opcode = OPC_3DSTATE_PIXEL_SHADER_PROGRAM;
    pixel_shader_program->dw0.retain = 0;
    pixel_shader_program->dw0.length = 23;
    /* dcl      t0.xy */
    inst = (unsigned int*)(++pixel_shader_program);
    i915_inst_decl(inst, REG_TYPE_T, T_TEX0, D0_CHANNEL_XY);
    /* dcl         t1.xy */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_T, T_TEX1, D0_CHANNEL_XY);
    /* dcl_2D   s0 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 0, D0_SAMPLE_TYPE_2D);
    /* dcl_2D   s1 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 1, D0_SAMPLE_TYPE_2D);
    /* dcl_2D   s2 */
    inst += 3;
    i915_inst_decl(inst, REG_TYPE_S, 2, D0_SAMPLE_TYPE_2D);
    /* texld    r0 t1 s0 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0); 
    src0 = UREG(REG_TYPE_T, 1); /* COORD */
    src1 = UREG(REG_TYPE_S, 0); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* texld    r0 t0 s1 */
    inst += 3;
    dest = UREG(REG_TYPE_R, 0); 
    src0 = UREG(REG_TYPE_T, 0); /* COORD */
    src1 = UREG(REG_TYPE_S, 1); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
    /* texld    oC t1 s2 */
    inst += 3;
    dest = UREG(REG_TYPE_OC, 0);
    src0 = UREG(REG_TYPE_T, 1); /* COORD */
    src1 = UREG(REG_TYPE_S, 2); /* SAMPLER */
    i915_inst_texld(inst, T0_TEXLD, dest, src0, src1);
}

_STATIC_ void i915_yuv2rgb_proc(XvMCSurface *surface)
{
    i915XvMCSurface *privSurface = (i915XvMCSurface *)surface->privData;
    i915XvMCContext *pI915XvMC = (i915XvMCContext *)privSurface->privContext;
    struct i915_3dstate_load_state_immediate_1 *load_state_immediate_1 = NULL;
    struct s2_dword *s2 = NULL;
    struct s3_dword *s3 = NULL;
    struct s4_dword *s4 = NULL;
    struct s5_dword *s5 = NULL;
    struct s6_dword *s6 = NULL;
    struct s7_dword *s7 = NULL;
    struct i915_3dstate_scissor_rectangle scissor_rectangle;
    struct i915_3dstate_load_indirect *load_indirect = NULL;
    sis_state *sis = NULL;
    ssb_state *ssb = NULL;
    msb_state *msb = NULL;
    psp_state *psp = NULL;
    struct i915_3dprimitive *_3dprimitive = NULL;
    struct vertex_data *vd = NULL;
    unsigned int size;
    void *base = NULL;

    /* 3DSTATE_LOAD_STATE_IMMEDIATE_1 */
    size = sizeof(*load_state_immediate_1) + sizeof(*s2) + sizeof(*s3) +
        sizeof(*s4) + sizeof(*s5) + sizeof(*s6) + sizeof(*s7);
    base = calloc(1, size);
    load_state_immediate_1 = (struct i915_3dstate_load_state_immediate_1 *)base;
    load_state_immediate_1->dw0.type = CMD_3D;
    load_state_immediate_1->dw0.opcode = OPC_3DSTATE_LOAD_STATE_IMMEDIATE_1;
    load_state_immediate_1->dw0.load_s2 = 1;
    load_state_immediate_1->dw0.load_s3 = 1;
    load_state_immediate_1->dw0.load_s4 = 1;
    load_state_immediate_1->dw0.load_s5 = 1;
    load_state_immediate_1->dw0.load_s6 = 1;
    load_state_immediate_1->dw0.load_s7 = 1;
    load_state_immediate_1->dw0.length = 5;

    s2 = (struct s2_dword *)(++load_state_immediate_1);
    s2->set0_texcoord_fmt = TEXCOORDFMT_2FP;
    s2->set1_texcoord_fmt = TEXCOORDFMT_2FP;
    s2->set2_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;
    s2->set3_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;
    s2->set4_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;
    s2->set5_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;
    s2->set6_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;
    s2->set7_texcoord_fmt = TEXCOORDFMT_NOT_PRESENT;

    s3 = (struct s3_dword *)(++s2);
    s4 = (struct s4_dword *)(++s3);
    s4->position_mask = VERTEXHAS_XY;
    s4->cull_mode = CULLMODE_NONE;
    s4->color_shade_mode = SHADEMODE_FLAT;
    s4->specular_shade_mode = SHADEMODE_FLAT;
    s4->fog_shade_mode = SHADEMODE_FLAT;
    s4->alpha_shade_mode = SHADEMODE_FLAT;
    s4->line_width = 0x2;     /* FIXME: 1.0??? */
    s4->point_width = 0x1; 

    s5 = (struct s5_dword *)(++s4);
    s6 = (struct s6_dword *)(++s5);
    s6->src_blend_factor = 1;
    s6->dest_blend_factor = 1;
    s6->color_buffer_write = 1;

    s7 = (struct s7_dword *)(++s6);
    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);

    /* 3DSTATE_3DSTATE_SCISSOR_RECTANGLE */
    scissor_rectangle.dw0.type = CMD_3D;
    scissor_rectangle.dw0.opcode = OPC_3DSTATE_SCISSOR_RECTANGLE;
    scissor_rectangle.dw0.length = 1;
    scissor_rectangle.dw1.min_x = 0;
    scissor_rectangle.dw1.min_y = 0;
    scissor_rectangle.dw2.max_x = 2047;
    scissor_rectangle.dw2.max_y = 2047;
    intelBatchbufferData(pI915XvMC, &scissor_rectangle, sizeof(scissor_rectangle), 0);

    /* 3DSTATE_LOAD_INDIRECT */
    size = sizeof(*load_indirect) + sizeof(*sis) + sizeof(*ssb) + sizeof(*msb) + sizeof(*psp);
    base = calloc(1, size);
    load_indirect = (struct i915_3dstate_load_indirect *)base;
    load_indirect->dw0.type = CMD_3D;
    load_indirect->dw0.opcode = OPC_3DSTATE_LOAD_INDIRECT;
    load_indirect->dw0.mem_select = 1;  /* Bearlake only */
    load_indirect->dw0.block_mask = BLOCK_SIS | BLOCK_SSB | BLOCK_MSB | BLOCK_PSP;
    load_indirect->dw0.length = 7;

    /* SIS */
    sis = (sis_state *)(++load_indirect);
    sis->dw0.valid = 1;
    sis->dw0.buffer_address = pI915XvMC->sis.offset;
    sis->dw1.length = ((sizeof(struct i915_3dstate_buffer_info) +
                        sizeof(struct i915_3dstate_dest_buffer_variables)) >> 2) - 1;

    /* SSB */
    ssb = (ssb_state *)(++sis);
    ssb->dw0.valid = 1;
    ssb->dw0.buffer_address = pI915XvMC->ssb.offset;
    ssb->dw1.length = ((sizeof(struct i915_3dstate_sampler_state) + 
                        sizeof(struct texture_sampler) * 3) >> 2) - 1;

    /* MSB */
    msb = (msb_state *)(++ssb);
    msb->dw0.valid = 1;
    msb->dw0.buffer_address = pI915XvMC->msb.offset;
    msb->dw1.length = ((sizeof(struct i915_3dstate_map_state) + 
                        sizeof(struct texture_map) * 3) >> 2) - 1;

    /* PSP */
    psp = (psp_state *)(++msb);
    psp->dw0.valid = 1;
    psp->dw0.buffer_address = pI915XvMC->psp.offset;
    psp->dw1.length = ((sizeof(struct i915_3dstate_pixel_shader_program) +
                        sizeof(union shader_inst)) >> 2) - 1;

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);

    /* 3DPRIMITIVE */
    size = sizeof(*_3dprimitive) + sizeof(*vd) * 3;
    base = calloc(1, size);
    _3dprimitive = (struct i915_3dprimitive *)base;
    _3dprimitive->dw0.inline_prim.type = CMD_3D;
    _3dprimitive->dw0.inline_prim.opcode = OPC_3DPRIMITIVE;
    _3dprimitive->dw0.inline_prim.vertex_location = VERTEX_INLINE;
    _3dprimitive->dw0.inline_prim.prim = PRIM_RECTLIST;
    _3dprimitive->dw0.inline_prim.length = size - 2;

    vd = (struct vertex_data *)(++_3dprimitive);
    vd->x = 0;          /* FIXME!!! */
    vd->x = 0;          /* FIXME */
    vd->tc0.tcx = 0;
    vd->tc0.tcy = 0;
    vd->tc1.tcx = 0;
    vd->tc1.tcy = 0;

    ++vd;
    vd->x = 0;          /* FIXME!!! */
    vd->x = 0;          /* FIXME */
    vd->tc0.tcx = 0;
    vd->tc0.tcy = 0;
    vd->tc1.tcx = 0;
    vd->tc1.tcy = 0;

    ++vd;
    vd->x = 0;          /* FIXME!!! */
    vd->x = 0;          /* FIXME */
    vd->tc0.tcx = 0;
    vd->tc0.tcy = 0;
    vd->tc1.tcx = 0;
    vd->tc1.tcy = 0;

    intelBatchbufferData(pI915XvMC, base, size, 0);
    free(base);
}

/***************************************************************************
// Function: i915_release_resource
// Description:
***************************************************************************/
_STATIC_ void i915_release_resource(Display *display, XvMCContext *context)
{
    i915XvMCContext *pI915XvMC;

    if (!display || !context)
        return;

    if (!(pI915XvMC = context->privData))
        return;

    pI915XvMC->ref--;
    i915_xvmc_unmap_buffers(pI915XvMC);

    driDestroyHashContents(pI915XvMC->drawHash);
    drmHashDestroy(pI915XvMC->drawHash);

    pthread_mutex_destroy(&pI915XvMC->ctxmutex);

    XLockDisplay(display);
    uniDRIDestroyContext(display, pI915XvMC->screen, pI915XvMC->id);
    XUnlockDisplay(display);

    intelDestroyBatchBuffer(pI915XvMC);
    drmUnmap(pI915XvMC->sarea_address, pI915XvMC->sarea_size);

    if (pI915XvMC->fd >= 0)
        drmClose(pI915XvMC->fd);
    pI915XvMC->fd = -1;

    XLockDisplay(display);
    uniDRICloseConnection(display, pI915XvMC->screen);
    _xvmc_destroy_context(display, context);
    XUnlockDisplay(display);

    free(pI915XvMC);
    context->privData = NULL;
}

/***************************************************************************
// Function: XvMCCreateContext
// Description: Create a XvMC context for the given surface parameters.
// Arguments:
//   display - Connection to the X server.
//   port - XvPortID to use as avertised by the X connection.
//   surface_type_id - Unique identifier for the Surface type.
//   width - Width of the surfaces.
//   height - Height of the surfaces.
//   flags - one or more of the following
//      XVMC_DIRECT - A direct rendered context is requested.
//
// Notes: surface_type_id and width/height parameters must match those
//        returned by XvMCListSurfaceTypes.
// Returns: Status
***************************************************************************/
Status XvMCCreateContext(Display *display, XvPortID port,
                         int surface_type_id, int width, int height, 
                         int flags, XvMCContext *context) 
{  
    i915XvMCContext *pI915XvMC = NULL;
    I915XvMCCreateContextRec *tmpComm = NULL;
    Status ret;
    drm_sarea_t *pSAREA;
    char *curBusID;
    uint *priv_data = NULL;
    uint magic;
    int major, minor;
    int priv_count;
    int isCapable;

    /* Verify Obvious things first */
    if (!display || !context)
        return BadValue;

    if (!(flags & XVMC_DIRECT)) {
        /* Indirect */
        XVMC_ERR("Indirect Rendering not supported! Using Direct.");
        return BadAccess;
    }

    /* Limit use to root for now */
    /* FIXME: remove it ??? */
/*
    if (geteuid()) {
        printf("Use of XvMC on i915 is currently limited to root\n");
        return BadAccess;
    }
*/
    /*
     *FIXME: Check $DISPLAY for legal values here
     */
    context->surface_type_id = surface_type_id;
    context->width = (unsigned short)((width + 15) & ~15);
    context->height = (unsigned short)((height + 15) & ~15);
    context->flags = flags;
    context->port = port;

    /* 
       Width, Height, and flags are checked against surface_type_id
       and port for validity inside the X server, no need to check
       here.
    */

    /* Verify the XvMC extension exists */
    XLockDisplay(display);
    if (!XvMCQueryExtension(display, &event_base, &error_base)) {
        XUnlockDisplay(display);
        XVMC_ERR("XvMCExtension is not available!");
        return BadAlloc;
    }
    /* Verify XvMC version */
    ret = XvMCQueryVersion(display, &major, &minor);
    if (ret) {
        XVMC_ERR("XvMCQueryVersion Failed, unable to determine protocol version.");
    }
    XUnlockDisplay(display);
    /* FIXME: Check Major and Minor here */

    /* Allocate private Context data */
    context->privData = (void *)calloc(1, sizeof(i915XvMCContext));
    if (!context->privData) {
        XVMC_ERR("Unable to allocate resources for XvMC context.");
        return BadAlloc;
    }
    pI915XvMC = (i915XvMCContext *)context->privData;

    /* Check for drm */
    if (!drmAvailable()) {
        XVMC_ERR("Direct Rendering is not avilable on this system!");
        free(pI915XvMC);
        context->privData = NULL;
        return BadAccess;
    }

    /*
      Pass control to the X server to create a drm_context_t for us and
      validate the with/height and flags.
    */
    XLockDisplay(display);
    if ((ret = _xvmc_create_context(display, context, &priv_count, &priv_data))) {
        XUnlockDisplay(display);
        XVMC_ERR("Unable to create XvMC Context.");
        free(pI915XvMC);
        context->privData = NULL;
        return ret;
    }
    XUnlockDisplay(display);

    if (priv_count != (sizeof(I915XvMCCreateContextRec) >> 2)) {
        XVMC_ERR("_xvmc_create_context() returned incorrect data size!");
        XVMC_INFO("\tExpected %d, got %d",
               (int)(sizeof(I915XvMCCreateContextRec) >> 2),priv_count);
        _xvmc_destroy_context(display, context);
        free(priv_data);
        free(pI915XvMC);
        context->privData = NULL;
        return BadAlloc;
    }

    tmpComm = (I915XvMCCreateContextRec *)priv_data;
    pI915XvMC->ctxno = tmpComm->ctxno;
    pI915XvMC->deviceID = tmpComm->deviceID;
    pI915XvMC->sis.handle = tmpComm->sis.handle;
    pI915XvMC->sis.offset = tmpComm->sis.offset;
    pI915XvMC->sis.size = tmpComm->sis.size;
    pI915XvMC->ssb.handle = tmpComm->ssb.handle;
    pI915XvMC->ssb.offset = tmpComm->ssb.offset;
    pI915XvMC->ssb.size = tmpComm->ssb.size;
    pI915XvMC->msb.handle = tmpComm->msb.handle;
    pI915XvMC->msb.offset = tmpComm->msb.offset;
    pI915XvMC->msb.size = tmpComm->msb.size;
    pI915XvMC->psp.handle = tmpComm->psp.handle;
    pI915XvMC->psp.offset = tmpComm->psp.offset;
    pI915XvMC->psp.size = tmpComm->psp.size;
    pI915XvMC->psc.handle = tmpComm->psc.handle;
    pI915XvMC->psc.offset = tmpComm->psc.offset;
    pI915XvMC->psc.size = tmpComm->psc.size;

    if (pI915XvMC->deviceID == PCI_CHIP_I915_G ||
        pI915XvMC->deviceID == PCI_CHIP_I915_GM ||
        pI915XvMC->deviceID == PCI_CHIP_I945_G ||
        pI915XvMC->deviceID == PCI_CHIP_I945_GM) {
        pI915XvMC->sis.bus_addr = tmpComm->sis.bus_addr;
        pI915XvMC->ssb.bus_addr = tmpComm->ssb.bus_addr;
        pI915XvMC->msb.bus_addr = tmpComm->msb.bus_addr;
        pI915XvMC->psp.bus_addr = tmpComm->psp.bus_addr;
        pI915XvMC->psc.bus_addr = tmpComm->psc.bus_addr;
    }

    pI915XvMC->corrdata.handle = tmpComm->corrdata.handle;
    pI915XvMC->corrdata.offset = tmpComm->corrdata.offset;
    pI915XvMC->corrdata.size = tmpComm->corrdata.size;
    pI915XvMC->batchbuffer.handle = tmpComm->batchbuffer.handle;
    pI915XvMC->batchbuffer.offset = tmpComm->batchbuffer.offset;
    pI915XvMC->batchbuffer.size = tmpComm->batchbuffer.size;
    pI915XvMC->sarea_size = tmpComm->sarea_size;
    pI915XvMC->sarea_priv_offset = tmpComm->sarea_priv_offset;
    pI915XvMC->screen = tmpComm->screen;
    pI915XvMC->depth = tmpComm->depth;

    /* Must free the private data we were passed from X */
    free(priv_data);
    priv_data = NULL;

    XLockDisplay(display);
    ret = uniDRIQueryDirectRenderingCapable(display, pI915XvMC->screen,
                                            &isCapable);
    if (!ret || !isCapable) {
        XUnlockDisplay(display);
	XVMC_ERR("Direct Rendering is not available on this system!");
        free(pI915XvMC);
        context->privData = NULL;
        return BadAlloc;
    }

    if (!uniDRIOpenConnection(display, pI915XvMC->screen,
                              &pI915XvMC->hsarea, &curBusID)) {
        XUnlockDisplay(display);
        XVMC_ERR("Could not open DRI connection to X server!");
        free(pI915XvMC);
        context->privData = NULL;
        return BadAlloc;
    }
    XUnlockDisplay(display);

    strncpy(pI915XvMC->busIdString, curBusID, 20);
    pI915XvMC->busIdString[20] = '\0';
    free(curBusID);

    /* Open DRI Device */
    if((pI915XvMC->fd = drmOpen(I915KernelDriverName, NULL)) < 0) {
        XVMC_ERR("DRM Device for %s could not be opened.", I915KernelDriverName);
        free(pI915XvMC);
        context->privData = NULL;
        return BadAccess;
    } /* !pI915XvMC->fd */

    /* Get magic number */
    drmGetMagic(pI915XvMC->fd, &magic);
    // context->flags = (unsigned long)magic;

    XLockDisplay(display);
    if (!uniDRIAuthConnection(display, pI915XvMC->screen, magic)) {
        XUnlockDisplay(display);
	XVMC_ERR("[XvMC]: X server did not allow DRI. Check permissions.");
        free(pI915XvMC);
        context->privData = NULL;
        return BadAlloc;
    }
    XUnlockDisplay(display);

    /*
     * Map DRI Sarea.
     */
    if (drmMap(pI915XvMC->fd, pI915XvMC->hsarea,
               pI915XvMC->sarea_size, &pI915XvMC->sarea_address) < 0) {
        XVMC_ERR("Unable to map DRI SAREA.\n");
        free(pI915XvMC);
        context->privData = NULL;
        return BadAlloc;
    }

    pSAREA = (drm_sarea_t *)pI915XvMC->sarea_address;
    pI915XvMC->driHwLock = (drmLock *)&pSAREA->lock;
    pI915XvMC->sarea = SAREAPTR(pI915XvMC);
    XLockDisplay(display);
    ret = XMatchVisualInfo(display, pI915XvMC->screen,
                           (pI915XvMC->depth == 32) ? 24 : pI915XvMC->depth, TrueColor,
                           &pI915XvMC->visualInfo);
    XUnlockDisplay(display);

    if (!ret) {
	XVMC_ERR("Could not find a matching TrueColor visual.");
        free(pI915XvMC);
        context->privData = NULL;
        drmUnmap(pI915XvMC->sarea_address, pI915XvMC->sarea_size);
        return BadAlloc;
    }

    if (!uniDRICreateContext(display, pI915XvMC->screen,
                             pI915XvMC->visualInfo.visual, &pI915XvMC->id,
                             &pI915XvMC->hHWContext)) {
        XVMC_ERR("Could not create DRI context.");
        free(pI915XvMC);
        context->privData = NULL;
        drmUnmap(pI915XvMC->sarea_address, pI915XvMC->sarea_size);
        return BadAlloc;
    }

    if (NULL == (pI915XvMC->drawHash = drmHashCreate())) {
	XVMC_ERR("Could not allocate drawable hash table.");
        free(pI915XvMC);
        context->privData = NULL;
        drmUnmap(pI915XvMC->sarea_address, pI915XvMC->sarea_size);
        return BadAlloc;
    }

    if (i915_xvmc_map_buffers(pI915XvMC)) {
        i915_xvmc_unmap_buffers(pI915XvMC);
        free(pI915XvMC);
        context->privData = NULL;
        drmUnmap(pI915XvMC->sarea_address, pI915XvMC->sarea_size);
        return BadAlloc;
    }

    /* Initialize private context values */
    pI915XvMC->yStride = STRIDE(width);
    pI915XvMC->uvStride = STRIDE(width >> 1);
    pI915XvMC->haveXv = 0;
    pI915XvMC->dual_prime = 0;
    pI915XvMC->last_flip = 0;
    pI915XvMC->locked = 0;
    pI915XvMC->port = context->port;
    pthread_mutex_init(&pI915XvMC->ctxmutex, NULL);
    intelInitBatchBuffer(pI915XvMC);
    pI915XvMC->ref = 1;
    return Success;
}

/***************************************************************************
// Function: XvMCDestroyContext
// Description: Destorys the specified context.
//
// Arguments:
//   display - Specifies the connection to the server.
//   context - The context to be destroyed.
//
// Returns: Status
***************************************************************************/
Status XvMCDestroyContext(Display *display, XvMCContext *context)
{
    i915XvMCContext *pI915XvMC;

    if (!display || !context)
        return BadValue;

    if (!(pI915XvMC = context->privData))
        return (error_base + XvMCBadContext);

    /* Pass Control to the X server to destroy the drm_context_t */
    i915_release_resource(display,context);
    return Success;
}

/***************************************************************************
// Function: XvMCCreateSurface
***************************************************************************/
Status XvMCCreateSurface(Display *display, XvMCContext *context, XvMCSurface *surface) 
{
    Status ret;
    i915XvMCContext *pI915XvMC;
    i915XvMCSurface *pI915Surface;
    I915XvMCCreateSurfaceRec *tmpComm = NULL;
    int priv_count;
    uint *priv_data;

    if (!display || !context || !display)
        return BadValue;

    if (!(pI915XvMC = context->privData))
        return (error_base + XvMCBadContext);

    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    surface->privData = (i915XvMCSurface *)malloc(sizeof(i915XvMCSurface));

    if (!(pI915Surface = surface->privData)) {
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAlloc;
    }

    /* Initialize private values */
    pI915Surface->last_render = 0;
    pI915Surface->last_flip = 0;
    pI915Surface->yStride = pI915XvMC->yStride;
    pI915Surface->uvStride = pI915XvMC->uvStride;
    pI915Surface->width = context->width;
    pI915Surface->height = context->height;
    pI915Surface->privContext = pI915XvMC;
    pI915Surface->privSubPic = NULL;
    pI915Surface->srf.map = NULL;
    XLockDisplay(display);

    if ((ret = _xvmc_create_surface(display, context, surface,
                                    &priv_count, &priv_data))) {
        XUnlockDisplay(display);
        XVMC_ERR("Unable to create XvMCSurface.");
        free(pI915Surface);
        surface->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return ret;
    }

    XUnlockDisplay(display);

    if (priv_count != (sizeof(I915XvMCCreateSurfaceRec) >> 2)) {
        XVMC_ERR("_xvmc_create_surface() returned incorrect data size!");
        XVMC_INFO("\tExpected %d, got %d",
               (int)(sizeof(I915XvMCCreateSurfaceRec) >> 2), priv_count);
        _xvmc_destroy_surface(display, surface);
        free(priv_data);
        free(pI915Surface);
        surface->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAlloc;
    }

    tmpComm = (I915XvMCCreateSurfaceRec *)priv_data;

    pI915Surface->srfNo = tmpComm->srfno;
    pI915Surface->srf.handle = tmpComm->srf.handle;
    pI915Surface->srf.offset = tmpComm->srf.offset;
    pI915Surface->srf.size = tmpComm->srf.size;
    free(priv_data);

    if (drmMap(pI915XvMC->fd,
               pI915Surface->srf.handle,
               pI915Surface->srf.size,
               (drmAddress *)&pI915Surface->srf.map) != 0) {
        _xvmc_destroy_surface(display, surface);
        free(pI915Surface);
        surface->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAlloc;
    }

    pI915XvMC->ref++;
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
    return Success;
}


/***************************************************************************
// Function: XvMCDestroySurface
***************************************************************************/
Status XvMCDestroySurface(Display *display, XvMCSurface *surface) 
{
    i915XvMCSurface *pI915Surface;
    i915XvMCContext *pI915XvMC;

    if (!display || !surface)
        return BadValue;

    if (!(pI915Surface = surface->privData))
        return (error_base + XvMCBadSurface);

    if (!(pI915XvMC = pI915Surface->privContext))
        return (error_base + XvMCBadSurface);

    if (pI915Surface->last_flip)
        XvMCSyncSurface(display,surface);

    if (pI915Surface->srf.map)
        drmUnmap(pI915Surface->srf.map, pI915Surface->srf.size);

    XLockDisplay(display);
    _xvmc_destroy_surface(display, surface);
    XUnlockDisplay(display);

    free(pI915Surface);
    surface->privData = NULL;
    pI915XvMC->ref--;

    return Success;
}

/***************************************************************************
// Function: XvMCCreateBlocks
***************************************************************************/
Status XvMCCreateBlocks(Display *display, XvMCContext *context,
                        unsigned int num_blocks, 
                        XvMCBlockArray *block) 
{
    if (!display || !context || !num_blocks || !block)
        return BadValue;

    memset(block, 0, sizeof(XvMCBlockArray));

    if (!(block->blocks = (short *)malloc(num_blocks << 6 * sizeof(short))))
        return BadAlloc;

    block->num_blocks = num_blocks;
    block->context_id = context->context_id;
    block->privData = NULL;

    return Success;
}

/***************************************************************************
// Function: XvMCDestroyBlocks
***************************************************************************/
Status XvMCDestroyBlocks(Display *display, XvMCBlockArray *block) 
{
    if (!display || block)
        return BadValue;

    if (block->blocks)
        free(block->blocks);

    block->context_id = 0;
    block->num_blocks = 0;
    block->blocks = NULL;
    block->privData = NULL;

    return Success;
}

/***************************************************************************
// Function: XvMCCreateMacroBlocks
***************************************************************************/
Status XvMCCreateMacroBlocks(Display *display, XvMCContext *context,
                             unsigned int num_blocks,
                             XvMCMacroBlockArray *blocks) 
{
    if (!display || !context || !blocks || !num_blocks)
        return BadValue;

    memset(blocks, 0, sizeof(XvMCMacroBlockArray));
    blocks->macro_blocks = (XvMCMacroBlock *)malloc(num_blocks * sizeof(XvMCMacroBlock));

    if (!blocks->macro_blocks)
        return BadAlloc;

    blocks->num_blocks = num_blocks;
    blocks->context_id = context->context_id;
    blocks->privData = NULL;

    return Success;
}

/***************************************************************************
// Function: XvMCDestroyMacroBlocks
***************************************************************************/
Status XvMCDestroyMacroBlocks(Display *display, XvMCMacroBlockArray *block) 
{
    if (!display || !block)
        return BadValue;
    if (block->macro_blocks)
        free(block->macro_blocks);

    block->context_id = 0;
    block->num_blocks = 0;
    block->macro_blocks = NULL;
    block->privData = NULL;

    return Success;
}

/***************************************************************************
// Function: XvMCRenderSurface
// Description: This function does the actual HWMC. Given a list of
//  macroblock structures it dispatched the hardware commands to execute
//  them. 
***************************************************************************/
Status XvMCRenderSurface(Display *display, XvMCContext *context,
                         unsigned int picture_structure,
                         XvMCSurface *target_surface,
                         XvMCSurface *past_surface,
                         XvMCSurface *future_surface,
                         unsigned int flags,
                         unsigned int num_macroblocks,
                         unsigned int first_macroblock,
                         XvMCMacroBlockArray *macroblock_array,
                         XvMCBlockArray *blocks) 
{
    int i;
    int picture_coding_type = MPEG_I_PICTURE;
    /* correction data buffer */
    char *corrdata_ptr;
    int corrdata_size = 0;

    /* Block Pointer */
    short *block_ptr;
    /* Current Macroblock Pointer */
    XvMCMacroBlock *mb;

    i915XvMCSurface *privTarget = NULL;
    i915XvMCSurface *privFuture = NULL;
    i915XvMCSurface *privPast = NULL;
    i915XvMCContext *pI915XvMC = NULL;

    /* Check Parameters for validity */
    if (!display || !context || !target_surface) {
        XVMC_ERR("Invalid Display, Context or Target!");
        return BadValue;
    }

    if (!num_macroblocks)
        return Success;

    if (!macroblock_array || !blocks) {
        XVMC_ERR("Invalid block data!");
        return BadValue;
    }

    if (macroblock_array->num_blocks < (num_macroblocks + first_macroblock)) {
        XVMC_ERR("Too many macroblocks requested for MB array size.");
        return BadValue;
    }

    if (!(pI915XvMC = context->privData))
        return (error_base + XvMCBadContext);

    if (!(privTarget = target_surface->privData))
        return (error_base + XvMCBadSurface);

    /* Test For YV12 Surface */
    if (context->surface_type_id != FOURCC_YV12) {
        XVMC_ERR("HWMC only possible on YV12 Surfaces.");
        return BadValue;
    }

    /* P Frame Test */
    if (!past_surface) {
        /* Just to avoid some ifs later. */
        privPast = privTarget;
    } else {
        if (!(privPast = past_surface->privData)) {
            XVMC_ERR("Invalid Past Surface!");
            return (error_base + XvMCBadSurface);
        }
        
        picture_coding_type = MPEG_P_PICTURE;
    }

    /* B Frame Test */
    if (!future_surface) {
        privFuture = privPast; // privTarget;
    } else {
        if (!past_surface) {
            XVMC_ERR("No Past Surface!");
            return BadValue;
        }

        if (!(privFuture = future_surface->privData)) {
            XVMC_ERR("Invalid Future Surface!");
            return (error_base + XvMCBadSurface);
        }

        picture_coding_type = MPEG_B_PICTURE;
    }

    LOCK_HARDWARE(pI915XvMC);
    corrdata_ptr = pI915XvMC->corrdata.map;
    corrdata_size = 0;

    for (i = first_macroblock; i < (num_macroblocks + first_macroblock); i++) {
        int bspm = 0;
        mb = &macroblock_array->macro_blocks[i];
        block_ptr = &(blocks->blocks[mb->index << 6]);

        /* Lockup can happen if the coordinates are too far out of range */
        if (mb->x > (target_surface->width >> 4)) {
            mb->x = 0;
            XVMC_INFO("reset x");
        }

        if (mb->y > (target_surface->height >> 4)) {
            mb->y = 0;
            XVMC_INFO("reset y");
        }

        /* Catch no pattern case */
        if (!(mb->macroblock_type & XVMC_MB_TYPE_PATTERN) &&
            !(mb->macroblock_type & XVMC_MB_TYPE_INTRA) &&
            mb->coded_block_pattern) {
            mb->coded_block_pattern = 0;
            XVMC_INFO("no coded blocks present!");
        }
        
        bspm = mb_bytes[mb->coded_block_pattern];

        if (!bspm)
            continue;

        corrdata_size += bspm;

        if (corrdata_size > pI915XvMC->corrdata.size) {
            XVMC_ERR("correction data buffer overflow.");
            break;
        }
        memcpy(corrdata_ptr, block_ptr, bspm);
        corrdata_ptr += bspm;
    } 

    i915_flush(pI915XvMC, 1, 0);
    // i915_mc_invalidate_subcontext_buffers(context, BLOCK_SIS | BLOCK_DIS | BLOCK_SSB 
    // | BLOCK_MSB | BLOCK_PSP | BLOCK_PSC);

    i915_mc_sampler_state_buffer(context);
    i915_mc_pixel_shader_program_buffer(context);
    i915_mc_pixel_shader_constants_buffer(context);
    i915_mc_one_time_state_initialization(context);

    i915_mc_static_indirect_state_buffer(context, target_surface, 
                                         picture_structure, flags,
                                         picture_coding_type);
    i915_mc_map_state_buffer(context, privTarget, privPast, privFuture);
    i915_mc_load_sis_msb_buffers(context);
    i915_mc_mpeg_set_origin(context, &macroblock_array->macro_blocks[first_macroblock]);

    for (i = first_macroblock; i < (num_macroblocks + first_macroblock); i++) {
        mb = &macroblock_array->macro_blocks[i];

        /* Intra Blocks */
        if (mb->macroblock_type & XVMC_MB_TYPE_INTRA) {
            i915_mc_mpeg_macroblock_ipicture(context, mb);
        } else if ((picture_structure & XVMC_FRAME_PICTURE) == XVMC_FRAME_PICTURE) { /* Frame Picture */
            switch (mb->motion_type & 3) {
            case XVMC_PREDICTION_FIELD: /* Field Based */
                i915_mc_mpeg_macroblock_2fbmv(context, mb, picture_structure);
                break;

            case XVMC_PREDICTION_FRAME: /* Frame Based */
                i915_mc_mpeg_macroblock_1fbmv(context, mb);
                break;

            case XVMC_PREDICTION_DUAL_PRIME:    /* Dual Prime */
                i915_mc_mpeg_macroblock_2fbmv(context, mb, picture_structure);
                break;

            default:    /* No Motion Type */
                renderError();
                break;
            }   
        } else {        /* Frame Picture */
            switch (mb->motion_type & 3) {
            case XVMC_PREDICTION_FIELD: /* Field Based */
                i915_mc_mpeg_macroblock_1fbmv(context, mb);
                break;

            case XVMC_PREDICTION_16x8:  /* 16x8 MC */
                i915_mc_mpeg_macroblock_2fbmv(context, mb, picture_structure);
                break;
                
            case XVMC_PREDICTION_DUAL_PRIME:    /* Dual Prime */
                i915_mc_mpeg_macroblock_1fbmv(context, mb);
                break;

            default:    /* No Motion Type */
                renderError();
                break;
            }
        }       /* Field Picture */
    }

    intelFlushBatch(pI915XvMC, TRUE);
    pI915XvMC->last_render = pI915XvMC->alloc.irq_emitted;
    privTarget->last_render = pI915XvMC->last_render;

    UNLOCK_HARDWARE(pI915XvMC);
    return Success;
}

/***************************************************************************
// Function: XvMCPutSurface
// Description:
// Arguments:
//  display: Connection to X server
//  surface: Surface to be displayed
//  draw: X Drawable on which to display the surface
//  srcx: X coordinate of the top left corner of the region to be
//          displayed within the surface.
//  srcy: Y coordinate of the top left corner of the region to be
//          displayed within the surface.
//  srcw: Width of the region to be displayed.
//  srch: Height of the region to be displayed.
//  destx: X cordinate of the top left corner of the destination region
//         in the drawable coordinates.
//  desty: Y cordinate of the top left corner of the destination region
//         in the drawable coordinates.
//  destw: Width of the destination region.
//  desth: Height of the destination region.
//  flags: One or more of the following.
//     XVMC_TOP_FIELD - Display only the Top field of the surface.
//     XVMC_BOTTOM_FIELD - Display only the Bottom Field of the surface.
//     XVMC_FRAME_PICTURE - Display both fields or frame.
//
// Info: Portions of this function derived from i915_video.c (XFree86)
//
//   This function is organized so that we wait as long as possible before
//   touching the overlay registers. Since we don't know that the last
//   flip has happened yet we want to give the overlay as long as
//   possible to catch up before we have to check on its progress. This
//   makes it unlikely that we have to wait on the last flip.
***************************************************************************/
Status XvMCPutSurface(Display *display,XvMCSurface *surface,
                      Drawable draw, short srcx, short srcy,
                      unsigned short srcw, unsigned short srch,
                      short destx, short desty,
                      unsigned short destw, unsigned short desth,
                      int flags) 
{
    i915XvMCContext *pI915XvMC;
    i915XvMCSurface *pI915Surface;
    i915XvMCSubpicture *pI915SubPic;
    I915XvMCCommandBuffer buf;

    // drawableInfo *drawInfo;
    Status ret;

    if (!display || !surface)
        return BadValue;

    if (!(pI915Surface = surface->privData))
        return (error_base + XvMCBadSurface);

    if (!(pI915XvMC = pI915Surface->privContext))
        return (error_base + XvMCBadSurface);

    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    /*
    if (getDRIDrawableInfoLocked(pI915XvMC->drawHash, display,
                                 pI915XvMC->screen, draw, 0, pI915XvMC->fd, pI915XvMC->hHWContext,
                                 pI915XvMC->sarea_address, FALSE, &drawInfo, sizeof(*drawInfo))) {
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAccess;
    }
    */
    if (!pI915XvMC->haveXv) {
        pI915XvMC->xvImage =
            XvCreateImage(display, pI915XvMC->port, FOURCC_XVMC,
                          (char *)&buf, pI915Surface->width, pI915Surface->height);
        pI915XvMC->gc = XCreateGC(display, draw, 0, 0);
        pI915XvMC->haveXv = 1;
    }

    pI915XvMC->draw = draw;
    pI915XvMC->xvImage->data = (char *)&buf;

    buf.command = INTEL_XVMC_COMMAND_DISPLAY;
    buf.ctxNo = pI915XvMC->ctxno;
    buf.srfNo = pI915Surface->srfNo;
    pI915SubPic = pI915Surface->privSubPic;
    buf.subPicNo = (!pI915SubPic ? 0 : pI915SubPic->srfNo);
    buf.real_id = FOURCC_YV12;

    XLockDisplay(display);

    if ((ret = XvPutImage(display, pI915XvMC->port, draw, pI915XvMC->gc,
                          pI915XvMC->xvImage, srcx, srcy, srcw, srch,
                          destx, desty, destw, desth))) {
        XUnlockDisplay(display);
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);

        return ret;
    }

    XSync(display, 0);
    XUnlockDisplay(display);
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);

    return Success;
}

/***************************************************************************
// Function: XvMCSyncSurface
// Arguments:
//   display - Connection to the X server
//   surface - The surface to synchronize
// Info:
// Returns: Status
***************************************************************************/
Status XvMCSyncSurface(Display *display, XvMCSurface *surface) 
{
    Status ret;
    int stat = 0;

    do {
        ret = XvMCGetSurfaceStatus(display, surface, &stat);
    } while (!ret && (stat & XVMC_RENDERING));

    return ret;
}

/***************************************************************************
// Function: XvMCFlushSurface
// Description:
//   This function commits pending rendering requests to ensure that they
//   wll be completed in a finite amount of time.
// Arguments:
//   display - Connection to X server
//   surface - Surface to flush
// Returns: Status
***************************************************************************/
Status XvMCFlushSurface(Display * display, XvMCSurface *surface) 
{
    return Success;
}

/***************************************************************************
// Function: XvMCGetSurfaceStatus
// Description:
// Arguments:
//  display: connection to X server
//  surface: The surface to query
//  stat: One of the Following
//    XVMC_RENDERING - The last XvMCRenderSurface command has not
//                     completed.
//    XVMC_DISPLAYING - The surface is currently being displayed or a
//                     display is pending.
***************************************************************************/
Status XvMCGetSurfaceStatus(Display *display, XvMCSurface *surface, int *stat) 
{
    i915XvMCSurface *pI915Surface;
    i915XvMCContext *pI915XvMC;

    if (!display || !surface || !stat)
        return BadValue;
    
    *stat = 0;

    if (!(pI915Surface = surface->privData))
        return (error_base + XvMCBadSurface);

    if (!(pI915XvMC = pI915Surface->privContext))
        return (error_base + XvMCBadSurface);

    // LOCK_HARDWARE(pI915XvMC);
    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    if (pI915Surface->last_flip) {
        /* This can not happen */
        if (pI915XvMC->last_flip < pI915Surface->last_flip) {
            XVMC_ERR("Context last flip is less than surface last flip.");
            PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
            return BadValue;
        }

        /*
          If the context has 2 or more flips after this surface it
          cannot be displaying. Don't bother to check.
        */
        if (!(pI915XvMC->last_flip > (pI915Surface->last_flip + 1))) {
            /*
              If this surface was the last flipped it is either displaying
              or about to be so don't bother checking.
            */
            if (pI915XvMC->last_flip == pI915Surface->last_flip) {
                *stat |= XVMC_DISPLAYING;
            }
        }
    }

    if (pI915Surface->last_render &&
        (pI915Surface->last_render > pI915XvMC->sarea->last_dispatch)) {
        *stat |= XVMC_RENDERING;
    }

    // UNLOCK_HARDWARE(pI915XvMC);
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
    return Success;
}

/***************************************************************************
// 
//  Surface manipulation functions
//
***************************************************************************/

/***************************************************************************
// Function: XvMCHideSurface
// Description: Stops the display of a surface.
// Arguments:
//   display - Connection to the X server.
//   surface - surface to be hidden.
//
// Returns: Status
***************************************************************************/
Status XvMCHideSurface(Display *display, XvMCSurface *surface) 
{
    i915XvMCSurface *pI915Surface;
    i915XvMCContext *pI915XvMC;
    int stat = 0, ret;

    if (!display || !surface)
        return BadValue;

    if (!(pI915Surface = surface->privData))
        return (error_base + XvMCBadSurface);

    /* Get the associated context pointer */
    if (!(pI915XvMC = pI915Surface->privContext))
        return (error_base + XvMCBadSurface);

    XvMCSyncSurface(display, surface);

    /*
      Get the status of the surface, if it is not currently displayed
      we don't need to worry about it.
    */
    if ((ret = XvMCGetSurfaceStatus(display, surface, &stat)) != Success)
        return ret;

    if (!(stat & XVMC_DISPLAYING))
        return Success;

    /* FIXME: */
    return Success;
}

/***************************************************************************
//
// Functions that deal with subpictures
//
***************************************************************************/



/***************************************************************************
// Function: XvMCCreateSubpicture
// Description: This creates a subpicture by filling out the XvMCSubpicture
//              structure passed to it and returning Success.
// Arguments:
//   display - Connection to the X server.
//   context - The context to create the subpicture for.
//   subpicture - Pre-allocated XvMCSubpicture structure to be filled in.
//   width - of subpicture
//   height - of subpicture
//   xvimage_id - The id describing the XvImage format.
//
// Returns: Status
***************************************************************************/
Status XvMCCreateSubpicture(Display *display, XvMCContext *context,
                            XvMCSubpicture *subpicture,
                            unsigned short width, unsigned short height,
                            int xvimage_id) 
{
    Status ret;
    i915XvMCContext *pI915XvMC;
    i915XvMCSubpicture *pI915Subpicture;
    I915XvMCCreateSurfaceRec *tmpComm = NULL;
    int priv_count;
    uint *priv_data;

    if (!subpicture || !context || !display)
        return BadValue;
  
    pI915XvMC = (i915XvMCContext *)context->privData;

    if (!pI915XvMC)
        return (error_base + XvMCBadContext);

    subpicture->privData =
        (i915XvMCSubpicture *)malloc(sizeof(i915XvMCSubpicture));

    if (!subpicture->privData)
        return BadAlloc;

    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    subpicture->context_id = context->context_id;
    subpicture->xvimage_id = xvimage_id;
    subpicture->width = width;
    subpicture->height = height;
    pI915Subpicture = (i915XvMCSubpicture *)subpicture->privData;

    XLockDisplay(display);
    if ((ret = _xvmc_create_subpicture(display, context, subpicture,
                                       &priv_count, &priv_data))) {
        XUnlockDisplay(display);
        XVMC_ERR("Unable to create XvMCSubpicture.");
        free(pI915Subpicture);
        subpicture->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return ret;
    }
    XUnlockDisplay(display);

    if (priv_count != (sizeof(I915XvMCCreateSurfaceRec) >> 2)) {
        XVMC_ERR("_xvmc_create_subpicture() returned incorrect data size!");
        XVMC_INFO("\tExpected %d, got %d", 
               (int)(sizeof(I915XvMCCreateSurfaceRec) >> 2), priv_count);
        XLockDisplay(display);
        _xvmc_destroy_subpicture(display, subpicture);
        XUnlockDisplay(display);
        free(priv_data);
        free(pI915Subpicture);
        subpicture->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAlloc;
    }

    tmpComm = (I915XvMCCreateSurfaceRec *)priv_data;
    pI915Subpicture->srfNo = tmpComm->srfno;
    pI915Subpicture->srf.handle = tmpComm->srf.handle;
    pI915Subpicture->srf.offset = tmpComm->srf.offset;
    pI915Subpicture->srf.size = tmpComm->srf.size;
    free(priv_data);

    if (drmMap(pI915XvMC->fd,
               pI915Subpicture->srf.handle,
               pI915Subpicture->srf.size,
               (drmAddress *)&pI915Subpicture->srf.map) != 0) {
        XLockDisplay(display);
        _xvmc_destroy_subpicture(display, subpicture);
        XUnlockDisplay(display);
        free(pI915Subpicture);
        subpicture->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadAlloc;
    }

    /* subpicture */
    subpicture->num_palette_entries = I915_SUBPIC_PALETTE_SIZE;
    subpicture->entry_bytes = 3;
    strncpy(subpicture->component_order, "YUV", 4);

    /* Initialize private values */
    pI915Subpicture->privContext = pI915XvMC;
    pI915Subpicture->last_render= 0;
    pI915Subpicture->last_flip = 0;
    pI915Subpicture->pitch = ((subpicture->width + 3) & ~3);

    switch(subpicture->xvimage_id) {
    case FOURCC_IA44:
    case FOURCC_AI44:
        break;

    default:
        drmUnmap(pI915Subpicture->srf.map, pI915Subpicture->srf.size);
        XLockDisplay(display);
        _xvmc_destroy_subpicture(display, subpicture);
        XUnlockDisplay(display);
        free(pI915Subpicture);
        subpicture->privData = NULL;
        PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
        return BadMatch;
    }

    pI915XvMC->ref++;
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
    return Success;
}

/***************************************************************************
// Function: XvMCClearSubpicture
// Description: Clear the area of the given subpicture to "color".
//              structure passed to it and returning Success.
// Arguments:
//   display - Connection to the X server.
//   subpicture - Subpicture to clear.
//   x, y, width, height - rectangle in the subpicture to clear.
//   color - The data to file the rectangle with.
//
// Returns: Status
***************************************************************************/
Status XvMCClearSubpicture(Display *display, XvMCSubpicture *subpicture,
                           short x, short y,
                           unsigned short width, unsigned short height,
                           unsigned int color) 
{
    i915XvMCContext *pI915XvMC;
    i915XvMCSubpicture *pI915Subpicture;

    if (!display || !subpicture)
        return BadValue;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    if (!(pI915XvMC = pI915Subpicture->privContext))
        return (error_base + XvMCBadSubpicture);

    if ((x < 0) || (x + width) > subpicture->width)
        return BadValue;

    if ((y < 0) || (y + height) > subpicture->height)
        return BadValue;

    /* FIXME: clear the area */

    return Success;
}

/***************************************************************************
// Function: XvMCCompositeSubpicture
// Description: Composite the XvImae on the subpicture. This composit uses
//              non-premultiplied alpha. Destination alpha is utilized
//              except for with indexed subpictures. Indexed subpictures
//              use a simple "replace".
// Arguments:
//   display - Connection to the X server.
//   subpicture - Subpicture to clear.
//   image - the XvImage to be used as the source of the composite.
//   srcx, srcy, width, height - The rectangle from the image to be used.
//   dstx, dsty - location in the subpicture to composite the source.
//
// Returns: Status
***************************************************************************/
Status XvMCCompositeSubpicture(Display *display, XvMCSubpicture *subpicture,
                               XvImage *image,
                               short srcx, short srcy,
                               unsigned short width, unsigned short height,
                               short dstx, short dsty) 
{
    i915XvMCContext *pI915XvMC;
    i915XvMCSubpicture *pI915Subpicture;

    if (!display || !subpicture)
        return BadValue;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    if (!(pI915XvMC = pI915Subpicture->privContext))
        return (error_base + XvMCBadSubpicture);

    if ((srcx < 0) || (srcx + width) > subpicture->width)
        return BadValue;

    if ((srcy < 0) || (srcy + height) > subpicture->height)
        return BadValue;

    if ((dstx < 0) || (dstx + width) > subpicture->width)
        return BadValue;

    if ((dsty < 0) || (dsty + width) > subpicture->height)
        return BadValue;

    if (image->id != subpicture->xvimage_id)
        return BadMatch;

    /* FIXME */
    return Success;
}


/***************************************************************************
// Function: XvMCDestroySubpicture
// Description: Destroys the specified subpicture.
// Arguments:
//   display - Connection to the X server.
//   subpicture - Subpicture to be destroyed.
//
// Returns: Status
***************************************************************************/
Status XvMCDestroySubpicture(Display *display, XvMCSubpicture *subpicture) 
{
    i915XvMCSubpicture *pI915Subpicture;
    i915XvMCContext *pI915XvMC;

    if (!display || !subpicture)
        return BadValue;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    if (!(pI915XvMC = pI915Subpicture->privContext))
        return (error_base + XvMCBadSubpicture);

    if (pI915Subpicture->last_render)
        XvMCSyncSubpicture(display, subpicture);

    if (pI915Subpicture->srf.map)
        drmUnmap(pI915Subpicture->srf.map, pI915Subpicture->srf.size);

    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    XLockDisplay(display);
    _xvmc_destroy_subpicture(display,subpicture);
    XUnlockDisplay(display);

    free(pI915Subpicture);
    subpicture->privData = NULL;
    pI915XvMC->ref--;
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);

    return Success;
}


/***************************************************************************
// Function: XvMCSetSubpicturePalette
// Description: Set the subpictures palette
// Arguments:
//   display - Connection to the X server.
//   subpicture - Subpiture to set palette for.
//   palette - A pointer to an array holding the palette data. The array
//     is num_palette_entries * entry_bytes in size.
// Returns: Status
***************************************************************************/

Status XvMCSetSubpicturePalette(Display *display, XvMCSubpicture *subpicture,
                                unsigned char *palette) 
{
    i915XvMCSubpicture *pI915Subpicture;
    int i, j;

    if (!display || !subpicture)
        return BadValue;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    j = 0;
    for (i = 0; i < 16; i++) {
        pI915Subpicture->palette[0][i] = palette[j++];
        pI915Subpicture->palette[1][i] = palette[j++];
        pI915Subpicture->palette[2][i] = palette[j++];
    }

    /* FIXME: Update the subpicture with the new palette */
    return Success;
}

/***************************************************************************
// Function: XvMCBlendSubpicture
// Description: 
//    The behavior of this function is different depending on whether
//    or not the XVMC_BACKEND_SUBPICTURE flag is set in the XvMCSurfaceInfo.
//    i915 only support frontend behavior.
//  
//    XVMC_BACKEND_SUBPICTURE not set ("frontend" behavior):
//   
//    XvMCBlendSubpicture is a no-op in this case.
//   
// Arguments:
//   display - Connection to the X server.
//   subpicture - The subpicture to be blended into the video.
//   target_surface - The surface to be displayed with the blended subpic.
//   source_surface - Source surface prior to blending.
//   subx, suby, subw, subh - The rectangle from the subpicture to use.
//   surfx, surfy, surfw, surfh - The rectangle in the surface to blend
//      blend the subpicture rectangle into. Scaling can ocure if 
//      XVMC_SUBPICTURE_INDEPENDENT_SCALING is set.
//
// Returns: Status
***************************************************************************/
Status XvMCBlendSubpicture(Display *display, XvMCSurface *target_surface,
                           XvMCSubpicture *subpicture,
                           short subx, short suby,
                           unsigned short subw, unsigned short subh,
                           short surfx, short surfy,
                           unsigned short surfw, unsigned short surfh) 
{
    i915XvMCSubpicture *pI915Subpicture;
    i915XvMCSurface *privTargetSurface;

    if (!display || !target_surface)
        return BadValue;

    if (!(privTargetSurface = target_surface->privData))
        return (error_base + XvMCBadSurface);

    if (subpicture) {
        if (!(pI915Subpicture = subpicture->privData))
            return (error_base + XvMCBadSubpicture);

        if ((FOURCC_AI44 != subpicture->xvimage_id) &&
            (FOURCC_IA44 != subpicture->xvimage_id))
            return (error_base + XvMCBadSubpicture);

        privTargetSurface->privSubPic = pI915Subpicture;
    } else {
        privTargetSurface->privSubPic = NULL;
    }

    return Success;
}

/***************************************************************************
// Function: XvMCBlendSubpicture2
// Description: 
//    The behavior of this function is different depending on whether
//    or not the XVMC_BACKEND_SUBPICTURE flag is set in the XvMCSurfaceInfo.
//    i915 only supports frontend blending.
//  
//    XVMC_BACKEND_SUBPICTURE not set ("frontend" behavior):
//   
//    XvMCBlendSubpicture2 blends the source_surface and subpicture and
//    puts it in the target_surface.  This does not effect the status of
//    the source surface but will cause the target_surface to query
//    XVMC_RENDERING until the blend is completed.
//   
// Arguments:
//   display - Connection to the X server.
//   subpicture - The subpicture to be blended into the video.
//   target_surface - The surface to be displayed with the blended subpic.
//   source_surface - Source surface prior to blending.
//   subx, suby, subw, subh - The rectangle from the subpicture to use.
//   surfx, surfy, surfw, surfh - The rectangle in the surface to blend
//      blend the subpicture rectangle into. Scaling can ocure if 
//      XVMC_SUBPICTURE_INDEPENDENT_SCALING is set.
//
// Returns: Status
***************************************************************************/
Status XvMCBlendSubpicture2(Display *display, 
                            XvMCSurface *source_surface,
                            XvMCSurface *target_surface,
                            XvMCSubpicture *subpicture,
                            short subx, short suby,
                            unsigned short subw, unsigned short subh,
                            short surfx, short surfy,
                            unsigned short surfw, unsigned short surfh)
{
    i915XvMCContext *pI915XvMC;
    i915XvMCSubpicture *pI915Subpicture;
    i915XvMCSurface *privSourceSurface;
    i915XvMCSurface *privTargetSurface;

    if (!display || !source_surface || !target_surface)
        return BadValue;

    if (!(privSourceSurface = source_surface->privData))
        return (error_base + XvMCBadSurface);

    if (!(privTargetSurface = target_surface->privData))
        return (error_base + XvMCBadSurface);

    if (!(pI915XvMC = privTargetSurface->privContext))
        return (error_base + XvMCBadSurface);

    if (((surfx + surfw) > privTargetSurface->width) ||
        ((surfy + surfh) > privTargetSurface->height))
        return BadValue;

    if ((privSourceSurface->width != privTargetSurface->width) ||
        (privTargetSurface->height != privTargetSurface->height))
        return BadValue;

    if (XvMCSyncSurface(display, source_surface))
        return BadValue;

    /* FIXME: update Target Surface */

    if (subpicture) {
        if (((subx + subw) > subpicture->width) ||
            ((suby + subh) > subpicture->height))
            return BadValue;

        if (!(pI915Subpicture = subpicture->privData))
            return (error_base + XvMCBadSubpicture);

        if ((FOURCC_AI44 != subpicture->xvimage_id) &&
            (FOURCC_IA44 != subpicture->xvimage_id))
            return (error_base + XvMCBadSubpicture);

        privTargetSurface->privSubPic = pI915Subpicture;
    } else {
        privTargetSurface->privSubPic = NULL;
    }

    return Success;
}

/***************************************************************************
// Function: XvMCSyncSubpicture
// Description: This function blocks until all composite/clear requests on
//              the subpicture have been complete.
// Arguments:
//   display - Connection to the X server.
//   subpicture - The subpicture to synchronize
//
// Returns: Status
***************************************************************************/
Status XvMCSyncSubpicture(Display *display, XvMCSubpicture *subpicture) 
{
    Status ret;
    int stat = 0;

    if (!display || !subpicture)
        return BadValue;

    do {
        ret = XvMCGetSubpictureStatus(display, subpicture, &stat);
    } while(!ret && (stat & XVMC_RENDERING));

    return ret;
}

/***************************************************************************
// Function: XvMCFlushSubpicture
// Description: This function commits pending composite/clear requests to
//              ensure that they will be completed in a finite amount of
//              time.
// Arguments:
//   display - Connection to the X server.
//   subpicture - The subpicture whos compsiting should be flushed
//
// Returns: Status
***************************************************************************/
Status XvMCFlushSubpicture(Display *display, XvMCSubpicture *subpicture) 
{
    i915XvMCSubpicture *pI915Subpicture;

    if (!display || !subpicture)
        return BadValue;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    return Success;
}

/***************************************************************************
// Function: XvMCGetSubpictureStatus
// Description: This function gets the current status of a subpicture
//
// Arguments:
//   display - Connection to the X server.
//   subpicture - The subpicture whos status is being queried
//   stat - The status of the subpicture. It can be any of the following
//          OR'd together:
//          XVMC_RENDERING  - Last composite or clear request not completed
//          XVMC_DISPLAYING - Suppicture currently being displayed.
//
// Returns: Status
***************************************************************************/
Status XvMCGetSubpictureStatus(Display *display, XvMCSubpicture *subpicture,
                               int *stat) 
{
    i915XvMCSubpicture *pI915Subpicture;
    i915XvMCContext *pI915XvMC;

    if (!display || !subpicture || stat)
        return BadValue;

    *stat = 0;

    if (!(pI915Subpicture = subpicture->privData))
        return (error_base + XvMCBadSubpicture);

    if (!(pI915XvMC = pI915Subpicture->privContext))
        return (error_base + XvMCBadSubpicture);

    // LOCK_HARDWARE(pI915XvMC);
    PPTHREAD_MUTEX_LOCK(pI915XvMC);
    /* FIXME: */
    if (pI915Subpicture->last_render &&
        (pI915Subpicture->last_render > pI915XvMC->sarea->last_dispatch)) {
        *stat |= XVMC_RENDERING;
    }

    // UNLOCK_HARDWARE(pI915XvMC);
    PPTHREAD_MUTEX_UNLOCK(pI915XvMC);
    return Success;
}

/***************************************************************************
// Function: XvMCQueryAttributes
// Description: An array of XvAttributes of size "number" is returned by
//   this function. If there are no attributes, NULL is returned and number
//   is set to 0. The array may be freed with xfree().
//
// Arguments:
//   display - Connection to the X server.
//   context - The context whos attributes we are querying.
//   number - The returned number of recognized atoms
//
// Returns:
//  An array of XvAttributes.
***************************************************************************/
XvAttribute *XvMCQueryAttributes(Display *display, XvMCContext *context,
                                 int *number) 
{
    /* now XvMC has no extra attribs than Xv */
    *number = 0;
    return NULL;
}

/***************************************************************************
// Function: XvMCSetAttribute
// Description: This function sets a context-specific attribute.
//
// Arguments:
//   display - Connection to the X server.
//   context - The context whos attributes we are querying.
//   attribute - The X atom of the attribute to be changed.
//   value - The new value for the attribute.
//
// Returns:
//  Status
***************************************************************************/
Status XvMCSetAttribute(Display *display, XvMCContext *context,
                        Atom attribute, int value)
{
    return Success;
}

/***************************************************************************
// Function: XvMCGetAttribute
// Description: This function queries a context-specific attribute and
//   returns the value.
//
// Arguments:
//   display - Connection to the X server.
//   context - The context whos attributes we are querying.
//   attribute - The X atom of the attribute to be queried
//   value - The returned attribute value
//
// Returns:
//  Status
// Notes:
***************************************************************************/
Status XvMCGetAttribute(Display *display, XvMCContext *context,
                        Atom attribute, int *value) 
{
    return Success;
}
