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

/*
 * naive frame codec: fixed-length frames with no markers. The encoder is a
 * pass-through - it copies bits unchanged, since a frame carries no preamble or
 * trailer. The decoder re-splits the stream into frames of `len` symbols and
 * reports the boundaries through get_state(), walking the frame state machine
 *
 *   OUTSIDE -> (BEGIN -> INSIDE -> END)*
 *
 * The preamble and trailer are zero-length, so BEGIN and END are pure 0-bit
 * transitions: a decode() call makes exactly one state move (consuming the next
 * frame's `len` payload symbols while INSIDE), and the caller drives decode() +
 * get_state() to observe each of OUTSIDE -> BEGIN -> INSIDE -> END. Once the first
 * frame opens the decoder never returns to OUTSIDE - frames run back to back. As a
 * pass-through it buffers nothing: every call consumes exactly the bits it writes,
 * so the caller advances `src` by the return value.
 */

#include <drifty/fc/naive.h>

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h> /* dt_malloc / dt_free / dt_memcpy */

static size_t min3(size_t a, size_t b, size_t c) {
  const size_t m = a < b ? a : b;
  return m < c ? m : c;
}

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  size_t len;   /* frame length, in symbols */
  int in_frame; /* whether a frame is currently open */
} naive_frame_encoder;

static int naive_encoder_begin(dt_frame_encoder *enc, dt_bit *dst,
                               size_t dst_len) {
  (void)enc;
  (void)dst;
  (void)dst_len;
  return 0; /* no preamble */
}

static int naive_encoder_begin_frame(dt_frame_encoder *enc, dt_bit *dst,
                                     size_t dst_len) {
  ((naive_frame_encoder *)enc->data)->in_frame = 1;
  (void)dst;
  (void)dst_len;
  return 0; /* a frame carries no marker */
}

static int naive_encoder_encode(dt_frame_encoder *enc, dt_bit *dst,
                                size_t dst_len, const dt_bit *src,
                                size_t src_len) {
  (void)enc;
  const size_t n = src_len < dst_len ? src_len : dst_len;
  dt_memcpy(dst, src, n); /* pass the bits through unchanged */
  return (int)n;
}

static int naive_encoder_end_frame(dt_frame_encoder *enc, dt_bit *dst,
                                   size_t dst_len) {
  ((naive_frame_encoder *)enc->data)->in_frame = 0;
  (void)dst;
  (void)dst_len;
  return 0; /* a frame carries no trailer */
}

static int naive_encoder_finalize(dt_frame_encoder *enc, dt_bit *dst,
                                  size_t dst_len) {
  (void)enc;
  (void)dst;
  (void)dst_len;
  return 0; /* nothing in flight */
}

dt_frame_encoder *dt_fc_naive_frame_encoder_create(size_t len) {
  dt_frame_encoder *enc = dt_malloc(sizeof(*enc));
  naive_frame_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  st->len = len;
  st->in_frame = 0;
  enc->begin = naive_encoder_begin;
  enc->begin_frame = naive_encoder_begin_frame;
  enc->encode = naive_encoder_encode;
  enc->end_frame = naive_encoder_end_frame;
  enc->finalize = naive_encoder_finalize;
  enc->data = st;
  return enc;
}

void dt_fc_naive_frame_encoder_destroy(dt_frame_encoder *enc) {
  if (!enc) {
    return;
  }
  dt_free(enc->data);
  dt_free(enc);
}

/* -- decoder --------------------------------------------------------------- */

typedef struct {
  size_t len;                   /* frame length, in symbols */
  size_t pos;                   /* symbols decoded in the current frame */
  dt_frame_decoder_state state; /* current frame state */
} naive_frame_decoder;

static int naive_decoder_begin(dt_frame_decoder *dec, const dt_bit *src,
                               size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0; /* no preamble to consume */
}

static dt_frame_decoder_state naive_decoder_get_state(dt_frame_decoder *dec) {
  return ((naive_frame_decoder *)dec->data)->state;
}

static int naive_decoder_decode(dt_frame_decoder *dec, dt_bit *dst,
                                size_t dst_len, const dt_bit *src,
                                size_t src_len) {
  naive_frame_decoder *st = dec->data;
  switch (st->state) {
  case DT_FRAME_DECODER_OUTSIDE:
  case DT_FRAME_DECODER_END:
    /* Open the next frame only when there is data for it - at end of stream the
     * decoder stays put rather than starting an empty frame. */
    if (src_len == 0) {
      return 0;
    }
    st->state = DT_FRAME_DECODER_BEGIN;
    return 0;
  case DT_FRAME_DECODER_BEGIN:
    st->state = DT_FRAME_DECODER_INSIDE;
    st->pos = 0;
    return 0;
  case DT_FRAME_DECODER_INSIDE: {
    const size_t n = min3(st->len - st->pos, dst_len, src_len);
    dt_memcpy(dst, src, n); /* pass the payload through unchanged */
    st->pos += n;
    if (st->pos == st->len) {
      st->state = DT_FRAME_DECODER_END;
    }
    return (int)n;
  }
  }
  return 0; /* unreachable */
}

static int naive_decoder_finalize(dt_frame_decoder *dec, dt_bit *dst,
                                  size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* nothing in flight */
}

dt_frame_decoder *dt_fc_naive_frame_decoder_create(size_t len) {
  dt_frame_decoder *dec = dt_malloc(sizeof(*dec));
  naive_frame_decoder *st = dt_malloc(sizeof(*st));
  if (!dec || !st) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->len = len;
  st->pos = 0;
  st->state = DT_FRAME_DECODER_OUTSIDE;
  dec->begin = naive_decoder_begin;
  dec->get_state = naive_decoder_get_state;
  dec->decode = naive_decoder_decode;
  dec->finalize = naive_decoder_finalize;
  dec->data = st;
  return dec;
}

void dt_fc_naive_frame_decoder_destroy(dt_frame_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_free(dec->data);
  dt_free(dec);
}

/* -- soft decoder ---------------------------------------------------------- */

typedef struct {
  size_t len;                   /* frame length, in symbols */
  size_t pos;                   /* symbols decoded in the current frame */
  dt_frame_decoder_state state; /* current frame state */
} naive_frame_soft_decoder;

static int naive_soft_decoder_begin(dt_frame_soft_decoder *dec,
                                    const dt_soft_bit *src, size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0; /* no preamble to consume */
}

static dt_frame_decoder_state naive_soft_decoder_get_state(
    dt_frame_soft_decoder *dec) {
  return ((naive_frame_soft_decoder *)dec->data)->state;
}

static int naive_soft_decoder_decode(dt_frame_soft_decoder *dec,
                                     dt_soft_bit *dst, size_t dst_len,
                                     const dt_soft_bit *src, size_t src_len) {
  naive_frame_soft_decoder *st = dec->data;
  switch (st->state) {
  case DT_FRAME_DECODER_OUTSIDE:
  case DT_FRAME_DECODER_END:
    if (src_len == 0) {
      return 0;
    }
    st->state = DT_FRAME_DECODER_BEGIN;
    return 0;
  case DT_FRAME_DECODER_BEGIN:
    st->state = DT_FRAME_DECODER_INSIDE;
    st->pos = 0;
    return 0;
  case DT_FRAME_DECODER_INSIDE: {
    const size_t n = min3(st->len - st->pos, dst_len, src_len);
    dt_memcpy(dst, src, n * sizeof(dt_soft_bit)); /* pass soft records through */
    st->pos += n;
    if (st->pos == st->len) {
      st->state = DT_FRAME_DECODER_END;
    }
    return (int)n;
  }
  }
  return 0; /* unreachable */
}

static int naive_soft_decoder_finalize(dt_frame_soft_decoder *dec,
                                       dt_soft_bit *dst, size_t dst_len) {
  (void)dec;
  (void)dst;
  (void)dst_len;
  return 0; /* nothing in flight */
}

dt_frame_soft_decoder *dt_fc_naive_frame_soft_decoder_create(size_t len) {
  dt_frame_soft_decoder *dec = dt_malloc(sizeof(*dec));
  naive_frame_soft_decoder *st = dt_malloc(sizeof(*st));
  if (!dec || !st) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->len = len;
  st->pos = 0;
  st->state = DT_FRAME_DECODER_OUTSIDE;
  dec->begin = naive_soft_decoder_begin;
  dec->get_state = naive_soft_decoder_get_state;
  dec->decode = naive_soft_decoder_decode;
  dec->finalize = naive_soft_decoder_finalize;
  dec->data = st;
  return dec;
}

void dt_fc_naive_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  dt_free(dec->data);
  dt_free(dec);
}
