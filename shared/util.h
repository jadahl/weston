/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2015 Red Hat Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#ifndef WESTON_SHARED_UTIL_H
#define WESTON_SHARED_UTIL_H

#include <math.h>

static inline double
wl_double_fixed_to_double(int32_t i, int32_t f)
{
        union {
                double d;
                int64_t i;
        } u;

        u.i = ((1023LL + (52LL - 31LL)) << 52) +  (1LL << 51) + f;

        return i + (u.d - (3LL << (52 - 32)));
}

static inline void
wl_double_fixed_from_double(double d, int32_t *i, int32_t *f)
{
        double integral;
        union {
                double d;
                int64_t i;
        } u;

        u.d = modf(d, &integral) + (3LL << (51 - 31));

        *i = integral;
        *f = u.i;
}

#endif /* WESTON_SHARED_UTIL_H */
