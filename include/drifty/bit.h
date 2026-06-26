/* clang-format off */
/*
 * MIT License
 *
 * Copyright (c) 2026 Robyn Kirkman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* clang-format on */

#ifndef DRIFTY_BIT_H
#define DRIFTY_BIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * dt_t - the bit symbol the whole drifty API speaks. Every input bit, coded
 * bit, and decoded bit is one byte holding one of the DT_ symbols below (one
 * bit per byte, never a packed bitfield).
 *
 * In practice you build and read three of them:
 *   DT_FALSE, DT_TRUE - a known 0 / 1.
 *   DT_ERASURE        - a position that exists but whose value is unknown (a
 *                       bit the channel lost). Valid as decoder input and may
 *                       appear in decoder output.
 * A decoder may additionally emit:
 *   DT_ABSENT         - a position it judges was deleted from the stream.
 *   DT_INVALID        - a bound value that is not a boolean.
 *
 * The symbols are points in a small flag space (the structural bits below), so
 * the predicates are plain mask tests. To recover a 0/1 from DT_FALSE/DT_TRUE,
 * guard with DT_IS_BIT() and take DT_BIT().
 */
typedef uint8_t dt_t; /* disposition of one bit position; uint8_t for
                         slot-struct footprint */

/* --- structural flag bits: each is one axis from the model --- */

/* decoder asserts the slot is missing (channel-claim, output only) */
#define DT_DELETED 0x10u
/* a slot exists in the transmit grid (sample/transmit domain) */
#define DT_PRESENT 0x08u
/* a value is specified for the slot */
#define DT_BOUND 0x04u
/* the bound value is a truth value */
#define DT_BOOLEAN 0x02u
/* the truth value itself, meaningful iff DT_BOOLEAN */
#define DT_VALUE 0x01u

/* --- the symbols, as points in that flag space --- */

/* uninitialized; never written */
#define DT_NONE 0x00u
/* inferred deletion / erasure-out */
#define DT_ABSENT (DT_DELETED)
/* present, no value bound */
#define DT_ERASURE (DT_PRESENT)
/* bound to a non-truth value */
#define DT_INVALID (DT_PRESENT | DT_BOUND)
/* bound, false */
#define DT_FALSE (DT_PRESENT | DT_BOUND | DT_BOOLEAN)
/* bound, true */
#define DT_TRUE (DT_PRESENT | DT_BOUND | DT_BOOLEAN | DT_VALUE)

/* --- predicates: each is a single mask test, naming one distinction --- */

/* a transmit-domain symbol (T, F, E, or I) - valid encoder/decoder input */
#define DT_IS_TRANSMIT(s) ((s) & DT_PRESENT)
/* an output-domain symbol: any transmit symbol or ABSENT; excludes NONE */
#define DT_IS_OUTPUT(s) ((s) & (DT_PRESENT | DT_DELETED))
/* carries a bound value (T, F, or I) - contributes value mass, not erasure */
#define DT_IS_BOUND(s) ((s) & DT_BOUND)
/* a usable boolean: T or F */
#define DT_IS_BIT(s) ((s) & DT_BOOLEAN)
/* extract the 0/1 payload; meaningful only when DT_IS_BIT(s) */
#define DT_BIT(s) ((s) & DT_VALUE)
/* marked deleted - an erasure to an outer code */
#define DT_IS_ABSENT(s) ((s) & DT_DELETED)

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_BIT_H */
