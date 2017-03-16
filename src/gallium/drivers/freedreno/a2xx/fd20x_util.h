/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2017 Wladimir J. van der Laan <laanwj@gmail.com>
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
 *    Rob Clark <robclark@freedesktop.org>
 *    Wladimir J. van der Laan <laanwj@gmail.com>
 */

#ifndef FD20x_UTIL_H_
#define FD20x_UTIL_H_

#include "fd2_context.h"

static inline void
fd20x_pre_draw(struct fd_batch *batch, bool indexed)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct fd2_context *fd2_ctx = fd2_context(batch->ctx);
	/* WL: wait for current DMA to finish?
	 * This is necessary for indexed rendering, I'm not sure it is necessary
	 * for non-indexed */
	OUT_PKT3(ring, CP_WAIT_REG_EQ, 4);
	OUT_RING(ring, 0x000005d0); /* RBBM_STATUS */
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00001000); /* bit: 12: VGT_BUSY_NO_DMA */
	OUT_RING(ring, 0x00000001);
	/* WL: dummy draw one triangle with indexes 0,0,0.
	 * with PRE_FETCH_CULL_ENABLE | GRP_CULL_ENABLE.
         * Not sure whether this is necessary at all.
         */
	OUT_PKT3(ring, CP_DRAW_INDX_BIN, 6);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x0003c004);
	OUT_RING(ring, 0x00000000);
	OUT_RING(ring, 0x00000003);
	OUT_RELOC(ring, fd_resource(fd2_ctx->solid_vertexbuf)->bo, 0x80, 0, 0);
	OUT_RING(ring, 0x00000006);
}

#endif
