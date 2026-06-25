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

#ifndef DRIFTY_HYBRID_DECODE_H
#define DRIFTY_HYBRID_DECODE_H

/* The decoder is built from a dt_code, and shares the result codes and bit
 * values defined alongside the encoder. */
#include <drifty/hybrid/encode.h>

#ifdef __cplusplus
extern "C" {
#endif

/* clang-format off */
/*
 * The receiver's half of drifty (the sender's half is in encode.h).
 *
 * Recover your original bits from a received stream: feed received bits to a
 * decoder and read your bits back, with flipped, inserted and dropped bits
 * corrected.
 *
 *   dt_stream_decoder *d = dt_stream_decoder_create(code, &(dt_stream_params){
 *       .decision_depth = 40,
 *       .max_drift      = 4,
 *       .p_flip = 0.01, .p_ins_true = 0.005, .p_ins_false = 0.005, .p_del = 0.01,
 *   });
 *   int n = dt_stream_decode(d, in, n_in, out, NULL, out_cap);
 *   while (dt_stream_decode_flush(d, out, out_cap) > 0) { }
 *
 *   dt_stream_decoder_destroy(d);
 *
 * `code` must be the same one the sender used, and must stay alive until the
 * decoder is freed. dt_stream_decoder is an opaque handle.
 */
/* clang-format on */

/*
 * In received data you may mark a bit DT_ERASURE to say "this one was lost";
 * the decoder then treats it as unknown instead of guessing 0 or 1. Ordinary
 * bits are DT_FALSE or DT_TRUE. All three bit values are defined in encode.h.
 */

/* ------------------------------------------------------------------------- */
/* Decoder                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Recovers your original bits from a received stream: corrects flipped bits and
 * keeps its place through inserted or dropped ones. Push received bits in, pull
 * decoded bits out, with a fixed delay.
 *
 * You may start at the beginning of a stream or join one mid-flight; either way
 * the first ~decision_depth decoded bits come out while the decoder is still
 * locking on, so discard them (or send a known preamble you can skip). Opaque
 * handle.
 */
typedef struct dt_stream_decoder dt_stream_decoder;

/* clang-format off */
/*
 * Decoder settings for dt_stream_decoder_create(). Use designated initializers;
 * any field you leave out is 0.
 *
 *   decision_depth : output delay, in bits, before each bit is committed. Bigger
 *                    is more reliable but slower to emit. Try ~6 * dt_code_k().
 *                    Required (must be >= 1).
 *   p_flip         : how often a coded bit is flipped, 0 < p_flip < 1 (e.g.
 *                    0.01 for 1%). Required.
 *   max_drift      : how far alignment may slip from inserted/dropped bits before
 *                    the decoder loses track. 0 (the default) corrects flipped
 *                    bits only; 4-8 also recovers from insertions and deletions.
 *   p_ins_true,
 *   p_ins_false,
 *   p_ins_erase    : how often a spurious DT_TRUE / DT_FALSE / DT_ERASURE bit is
 *                    inserted into the stream, per bit and at any position. Their
 *                    sum is the overall insertion rate; it sets how readily the
 *                    decoder realigns, while the split only biases which value it
 *                    expects an inserted bit to carry. So an even true/false split
 *                    behaves the same as one combined rate - set them unequal only
 *                    to favour one inserted value.
 *   p_del          : how often a coded bit is dropped, per bit and at any position.
 *                    The insertion rates and p_del together must sum to < 1, and
 *                    are required when max_drift > 0; leave 0 otherwise.
 *   p_ovr_true,
 *   p_ovr_false,
 *   p_ovr_erase    : how often a coded bit is overwritten with a fixed DT_TRUE /
 *                    DT_FALSE / DT_ERASURE, regardless of what was sent. The three
 *                    must sum to < 1 (the remainder is the chance the bit arrives
 *                    normally). All 0 (the default) if there are no overwrites;
 *                    p_ovr_erase doubles as the plain erasure rate.
 *
 * Rough probabilities are fine; only their relative sizes matter.
 */
/* clang-format on */
typedef struct {
  int decision_depth;
  int max_drift;
  double p_flip;
  double p_ins_true;
  double p_ins_false;
  double p_ins_erase;
  double p_del;
  double p_ovr_true;
  double p_ovr_false;
  double p_ovr_erase;
} dt_stream_params;

/*
 * Details about how an info-bit position was decoded.
 */
typedef struct {
  /*
   * All fields are consistencies, not probabilities, ranging 0...1.
   *
   * In the presence of information loss, c_true and c_false may sum
   * to more than 1.  Information loss includes erasures, deletions,
   * and stuck bits.
   */

  // Consistency of the proposition that the encoded bit was true
  double c_true;
  // Consistency of the proposition that the encoded bit was false
  double c_false;
  // Consistency of the proposition that the encoded bit is unrecoverable
  double c_lost;
  // Consistency of the proposition that the `dt_code` is correct.
  double c_lock;
} dt_decode_details;

/*
 * Make a decoder for `code` (which must stay alive until the decoder is freed)
 * using `params`. Returns NULL on invalid settings or out of memory; free it
 * with dt_stream_decoder_destroy().
 */
dt_stream_decoder *dt_stream_decoder_create(const dt_code *code,
                                            const dt_stream_params *params);

/* Free a decoder. Passing NULL is fine. */
void dt_stream_decoder_destroy(dt_stream_decoder *d);

/*
 * Feed `n_in` received bits (each DT_FALSE, DT_TRUE, or DT_ERASURE) and collect
 * up to `max_out` decoded bits into `out`. Returns how many decoded bits were
 * written (0 or more), or a negative DT_ERR_* code.
 *
 * You get about one decoded bit per dt_code_n(code) received bits. If `out`
 * fills up (return value == max_out), call again to collect more before feeding
 * more input.
 *
 * `out` and `details` may both be NULL.  If supplied, they must be arrays
 * of length max_out.
 *
 * Elements of `out` are one of DT_TRUE, DT_FALSE, or DT_ERASURE, whichever is
 * most consistent.
 *
 * `details` returns the inner state of the decoder at each decoded position.
 */
int dt_stream_decode(dt_stream_decoder *d, const uint8_t *in, int n_in,
                     uint8_t *out, dt_decode_details *details, int max_out);

/*
 * Call at the end of the stream to get the last decoded bits still in flight.
 * Returns how many bits were written (0..max_out); call it repeatedly until it
 * returns 0, after which every bit has been decoded.
 */
int dt_stream_decode_flush(dt_stream_decoder *d, uint8_t *out,
                           dt_decode_details *details, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DRIFTY_HYBRID_DECODE_H */
