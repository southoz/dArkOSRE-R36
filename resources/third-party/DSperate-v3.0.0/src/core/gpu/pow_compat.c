/* SPDX-License-Identifier: GPL-3.0-or-later */
/* DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors. */

/* Pins pow() to GLIBC_2.17 so aarch64 builds don't pick up glibc 2.29's
 * pow@GLIBC_2.29 as the runtime floor. Isolated in its own TU with -fno-lto:
 * .symver does not survive -flto=auto. */
#include <math.h>

#if defined(__aarch64__) && defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 29)
__asm__(".symver pow, pow@GLIBC_2.17");
#endif
#endif

double ds_pow_compat(double x, double y) { return pow(x, y); }
