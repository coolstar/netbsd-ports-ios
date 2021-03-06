/* 	$NetBSD: rasops4.c,v 1.23 2019/08/01 03:38:12 rin Exp $	*/

/*-
 * Copyright (c) 1999 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Andrew Doran.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: rasops4.c,v 1.23 2019/08/01 03:38:12 rin Exp $");

#include "opt_rasops.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <machine/endian.h>

#include <dev/wscons/wsdisplayvar.h>
#include <dev/wscons/wsconsio.h>

#define	_RASOPS_PRIVATE
#include <dev/rasops/rasops.h>
#include <dev/rasops/rasops_masks.h>

static void	rasops4_copycols(void *, int, int, int, int);
static void	rasops4_erasecols(void *, int, int, int, long);
static void	rasops4_do_cursor(struct rasops_info *);
static void	rasops4_putchar(void *, int, int col, u_int, long);
#ifndef RASOPS_SMALL
static void	rasops4_putchar8(void *, int, int col, u_int, long);
static void	rasops4_putchar12(void *, int, int col, u_int, long);
static void	rasops4_putchar16(void *, int, int col, u_int, long);
static void	rasops4_makestamp(struct rasops_info *, long);
#endif

/*
 * offset = STAMP_SHIFT(fontbits, nibble #) & STAMP_MASK
 * destination = STAMP_READ(offset)
 */
#define STAMP_SHIFT(fb, n)	((n) ? (fb) >> 4 : (fb))
#define STAMP_MASK		0xf
#define STAMP_READ(o)		stamp[o]

/*
 * Initialize rasops_info struct for this colordepth.
 */
void
rasops4_init(struct rasops_info *ri)
{

	if ((ri->ri_font->fontwidth & 1) != 0) {
		ri->ri_ops.erasecols = rasops4_erasecols;
		ri->ri_ops.copycols = rasops4_copycols;
		ri->ri_do_cursor = rasops4_do_cursor;
	}

	switch (ri->ri_font->fontwidth) {
#ifndef RASOPS_SMALL
	case 8:
		ri->ri_ops.putchar = rasops4_putchar8;
		break;
	case 12:
		ri->ri_ops.putchar = rasops4_putchar12;
		break;
	case 16:
		ri->ri_ops.putchar = rasops4_putchar16;
		break;
#endif	/* !RASOPS_SMALL */
	default:
		panic("fontwidth not 8/12/16 or RASOPS_SMALL - fixme!");
		ri->ri_ops.putchar = rasops4_putchar;
		return;
	}

#ifndef RASOPS_SMALL
	rasops_allocstamp(ri, sizeof(uint16_t) * 16);
#endif
}

/*
 * Put a single character. This is the generic version.
 */
static void
rasops4_putchar(void *cookie, int row, int col, u_int uc, long attr)
{

	/* XXX punt */
}

#ifndef RASOPS_SMALL
/*
 * Recompute the blitting stamp.
 */
static void
rasops4_makestamp(struct rasops_info *ri, long attr)
{
	uint16_t *stamp = (uint16_t *)ri->ri_stamp;
	int i, fg, bg;

	fg = ri->ri_devcmap[((uint32_t)attr >> 24) & 0xf] & 0xf;
	bg = ri->ri_devcmap[((uint32_t)attr >> 16) & 0xf] & 0xf;
	ri->ri_stamp_attr = attr;

	for (i = 0; i < 16; i++) {
#if BYTE_ORDER == BIG_ENDIAN
#define NEED_LITTLE_ENDIAN_STAMP RI_BSWAP
#else
#define NEED_LITTLE_ENDIAN_STAMP 0
#endif
		if ((ri->ri_flg & RI_BSWAP) == NEED_LITTLE_ENDIAN_STAMP) {
			/* little endian */
			stamp[i]  = (i & 1 ? fg : bg) << 12;
			stamp[i] |= (i & 2 ? fg : bg) << 8;
			stamp[i] |= (i & 4 ? fg : bg) << 4;
			stamp[i] |= (i & 8 ? fg : bg) << 0;
		} else {
			/* big endian */
			stamp[i]  = (i & 1 ? fg : bg) << 0;
			stamp[i] |= (i & 2 ? fg : bg) << 4;
			stamp[i] |= (i & 4 ? fg : bg) << 8;
			stamp[i] |= (i & 8 ? fg : bg) << 12;
		}
	}
}

#define	RASOPS_DEPTH	4

#define	RASOPS_WIDTH	8
#include "rasops_putchar_width.h"
#undef	RASOPS_WIDTH

#define	RASOPS_WIDTH	12
#include "rasops_putchar_width.h"
#undef	RASOPS_WIDTH

#define	RASOPS_WIDTH	16
#include "rasops_putchar_width.h"
#undef	RASOPS_WIDTH

#endif	/* !RASOPS_SMALL */

/*
 * Grab routines common to depths where (bpp < 8)
 */
#define NAME(ident)	rasops4_##ident
#define PIXEL_SHIFT	2

#include <dev/rasops/rasops_bitops.h>
