/* Copyright (c) 2000, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef _decimal_h
#define _decimal_h

#include <sys/types.h>
#include <stdint.h>

#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

#define likely(x)	__builtin_expect((x),1)
#define unlikely(x)	__builtin_expect((x),0)

typedef enum
{
    TRUNCATE=0,
    HALF_EVEN,
    HALF_UP,
    CEILING,
    FLOOR
} decimal_round_mode;

typedef int32_t decimal_digit_t;

/**
    intg is the number of *decimal* digits (NOT number of decimal_digit_t's !)
         before the point
    frac is the number of decimal digits after the point
    len  is the length of buf (length of allocated space) in decimal_digit_t's,
         not in bytes
    sign false means positive, true means negative
    buf  is an array of decimal_digit_t's
 */
typedef struct st_decimal_t {
  int intg, frac, len;
  char sign;
  decimal_digit_t *buf;
} decimal_t;

#ifdef __cplusplus
extern "C" {
#endif

int decimal_bin_size(int precision, int scale);
int decimal2string(const decimal_t *from, char *to, int *to_len,
                   int fixed_precision, int fixed_decimals,
                   char filler);

int bin2decimal(const u_char *from, decimal_t *to, int precision, int scale);

#ifdef __cplusplus
}
#endif

#define E_DEC_OK                0
#define E_DEC_TRUNCATED         1
#define E_DEC_OVERFLOW          2
#define E_DEC_DIV_ZERO          4
#define E_DEC_BAD_NUM           8
#define E_DEC_OOM              16

#define E_DEC_ERROR            31
#define E_DEC_FATAL_ERROR      30

#define decimal_make_zero(dec)        do {                \
                                        (dec)->buf[0]=0;    \
                                        (dec)->intg=1;      \
                                        (dec)->frac=0;      \
                                        (dec)->sign=0;      \
                                      } while(0)

#endif

