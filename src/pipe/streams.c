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
 * Encoder / decoder / soft-decoder pipes: dt_pipe adapters over the streaming
 * codecs (see streams.h). Each buffers hard input on its sink, runs the wrapped
 * codec in tick() over the whole buffered input, and appends the codec's output
 * to the buffer its source drains:
 *
 *   encoder      push info bits  -> tick encodes     -> pull coded bits
 *   decoder      push coded bits -> tick decodes     -> pull recovered bits
 *   soft_decoder push coded bits -> tick soft-decodes -> soft_pull records
 *
 * begin() primes the codec (encoder preamble; decoder begin with no preamble) and
 * resets the buffers; finalize() encodes/decodes any straggler input and then
 * flushes the codec's trailer / in-flight tail. The wrapped codec is not owned.
 * The dt_pipe is the first member, so destroy frees the pipe through it.
 */

#include <drifty/pipe/streams.h>

#include <drifty/bit.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>
#include <drifty/stream_decoder.h>
#include <drifty/stream_encoder.h>
#include <drifty/stream_soft_decoder.h>

#include "fifo.h"

/* Room reserved for a codec's begin/finalize output (preamble / trailer / the
 * decision-depth tail) - far above any drifty codec's need; grown if a codec
 * ever reports too little. */
#define STREAM_EDGE_ROOM 4096

/* -- shared no-op faces (an unused hard/soft face is callable, yields nothing) - */

static int noop_soft_push(dt_pipe_sink *s, const dt_soft_bit *x, size_t n) {
  (void)s; (void)x; (void)n; return 0;
}
static int noop_pull(dt_pipe_source *s, dt_bit *x, size_t n) {
  (void)s; (void)x; (void)n; return 0;
}
static int noop_soft_pull(dt_pipe_source *s, dt_soft_bit *x, size_t n) {
  (void)s; (void)x; (void)n; return 0;
}
static int noop_finish(dt_pipe_sink *s) {
  (void)s; return 0;
}

/* -- encoder pipe (info bits -> coded bits) -------------------------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_stream_encoder *enc; // not owned
  dt_bit_fifo in;         // buffered info bits
  dt_bit_fifo out;        // buffered coded bits
} enc_pipe;

static int enc_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  return dt_bit_fifo_append(&((enc_pipe *)sink->data)->in, src, src_len);
}
static int enc_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  return dt_bit_fifo_drain(&((enc_pipe *)source->data)->out, dst, dst_len);
}

/* Encode the whole input buffer into the output. encode() checks room up front
 * and writes nothing when short, so growing and retrying is safe. */
static int enc_run(enc_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  size_t room = n * 2 + 64;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, room) < 0) {
      return -1;
    }
    int w = c->enc->encode(c->enc, c->out.buf + c->out.tail, c->out.cap - c->out.tail,
                           c->in.buf + c->in.head, n);
    if (w >= 0) {
      c->out.tail += (size_t)w;
      c->in.head = c->in.tail = 0;
      return dt_fifo_clamp_int((size_t)w);
    }
    room *= 2;
  }
}

/* begin: run the encoder's begin phase, emitting any preamble into the output
 * buffer. It does not touch the input buffer - a stage's begin processes buffered
 * input, it does not discard it (the codec is (re)initialised by enc->begin). */
static int enc_begin(dt_pipe *pipe) {
  enc_pipe *c = (enc_pipe *)pipe->data;
  size_t room = STREAM_EDGE_ROOM;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, room) < 0) {
      return -1;
    }
    int w = c->enc->begin(c->enc, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
    if (w >= 0) {
      c->out.tail += (size_t)w;
      return 0;
    }
    room *= 2;
  }
}

static int enc_tick(dt_pipe *pipe) { return enc_run((enc_pipe *)pipe->data); }

static int enc_finalize(dt_pipe *pipe) {
  enc_pipe *c = (enc_pipe *)pipe->data;
  int a = enc_run(c);
  if (a < 0) {
    return a;
  }
  size_t room = STREAM_EDGE_ROOM;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, room) < 0) {
      return -1;
    }
    int w = c->enc->finalize(c->enc, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
    if (w >= 0) {
      c->out.tail += (size_t)w;
      return dt_fifo_clamp_int((size_t)a + (size_t)w);
    }
    room *= 2;
  }
}

static dt_pipe_source *enc_get_source(dt_pipe *pipe) {
  return &((enc_pipe *)pipe->data)->source;
}
static dt_pipe_sink *enc_get_sink(dt_pipe *pipe) {
  return &((enc_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_encoder_create(dt_stream_encoder *encoder) {
  if (!encoder) {
    return NULL;
  }
  enc_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = enc_get_source;
  c->base.sink = enc_get_sink;
  c->base.begin = enc_begin;
  c->base.tick = enc_tick;
  c->base.finalize = enc_finalize;
  c->base.data = c;
  c->source.pull = enc_pull;
  c->source.soft_pull = noop_soft_pull;
  c->source.data = c;
  c->sink.push = enc_push;
  c->sink.soft_push = noop_soft_push;
  c->sink.finish = noop_finish;
  c->sink.data = c;
  c->enc = encoder;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_encoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  enc_pipe *c = (enc_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

/* -- decoder pipe (coded bits -> recovered bits) --------------------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_stream_decoder *dec; // not owned
  dt_bit_fifo in;         // buffered received bits
  dt_bit_fifo out;        // buffered recovered bits
} dec_pipe;

static int dec_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  return dt_bit_fifo_append(&((dec_pipe *)sink->data)->in, src, src_len);
}
static int dec_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  return dt_bit_fifo_drain(&((dec_pipe *)source->data)->out, dst, dst_len);
}

/* Decode the whole input buffer into the output, draining the decoder's own
 * overflow buffer (a decode that fills dst has more buffered; re-call with no
 * input to drain it) per the stream_decoder contract. */
static int dec_run(dec_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  size_t produced = 0;
  int fed = 0;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, 512) < 0) {
      return -1;
    }
    size_t cap = c->out.cap - c->out.tail;
    int w = c->dec->decode(c->dec, c->out.buf + c->out.tail, cap,
                           fed ? NULL : (c->in.buf + c->in.head), fed ? (size_t)0 : n);
    if (w < 0) {
      return w;
    }
    c->out.tail += (size_t)w;
    produced += (size_t)w;
    fed = 1;
    if ((size_t)w < cap) {
      break; /* did not fill dst: nothing more buffered */
    }
  }
  c->in.head = c->in.tail = 0;
  return dt_fifo_clamp_int(produced);
}

/* begin: run the decoder's begin phase over whatever is already buffered on the
 * sink, consuming any preamble at its head and leaving the rest for tick. Does
 * not reset the buffers - buffered input is processed, not discarded. */
static int dec_begin(dt_pipe *pipe) {
  dec_pipe *c = (dec_pipe *)pipe->data;
  size_t n = c->in.tail - c->in.head;
  int consumed = c->dec->begin(c->dec, c->in.buf + c->in.head, n);
  if (consumed < 0) {
    return consumed;
  }
  c->in.head += (size_t)consumed;
  if (c->in.head == c->in.tail) {
    c->in.head = c->in.tail = 0;
  }
  return 0;
}

static int dec_tick(dt_pipe *pipe) { return dec_run((dec_pipe *)pipe->data); }

static int dec_finalize(dt_pipe *pipe) {
  dec_pipe *c = (dec_pipe *)pipe->data;
  int a = dec_run(c);
  if (a < 0) {
    return a;
  }
  /* Flush the decision-depth tail still in flight - far under STREAM_EDGE_ROOM. */
  if (dt_bit_fifo_reserve(&c->out, STREAM_EDGE_ROOM) < 0) {
    return -1;
  }
  int w = c->dec->finalize(c->dec, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
  if (w < 0) {
    return w;
  }
  c->out.tail += (size_t)w;
  return dt_fifo_clamp_int((size_t)a + (size_t)w);
}

static dt_pipe_source *dec_get_source(dt_pipe *pipe) {
  return &((dec_pipe *)pipe->data)->source;
}
static dt_pipe_sink *dec_get_sink(dt_pipe *pipe) {
  return &((dec_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_decoder_create(dt_stream_decoder *decoder) {
  if (!decoder) {
    return NULL;
  }
  dec_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = dec_get_source;
  c->base.sink = dec_get_sink;
  c->base.begin = dec_begin;
  c->base.tick = dec_tick;
  c->base.finalize = dec_finalize;
  c->base.data = c;
  c->source.pull = dec_pull;
  c->source.soft_pull = noop_soft_pull;
  c->source.data = c;
  c->sink.push = dec_push;
  c->sink.soft_push = noop_soft_push;
  c->sink.finish = noop_finish;
  c->sink.data = c;
  c->dec = decoder;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_decoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  dec_pipe *c = (dec_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

/* -- soft-decoder pipe (coded bits -> soft records) ------------------------ */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_stream_soft_decoder *dec; // not owned
  dt_bit_fifo in;              // buffered received bits
  dt_soft_fifo out;            // buffered soft records
} sdec_pipe;

static int sdec_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  return dt_bit_fifo_append(&((sdec_pipe *)sink->data)->in, src, src_len);
}
static int sdec_soft_pull(dt_pipe_source *source, dt_soft_bit *dst, size_t dst_len) {
  return dt_soft_fifo_drain(&((sdec_pipe *)source->data)->out, dst, dst_len);
}

static int sdec_run(sdec_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  size_t produced = 0;
  int fed = 0;
  for (;;) {
    if (dt_soft_fifo_reserve(&c->out, 512) < 0) {
      return -1;
    }
    size_t cap = c->out.cap - c->out.tail;
    int w = c->dec->decode(c->dec, c->out.buf + c->out.tail, cap,
                           fed ? NULL : (c->in.buf + c->in.head), fed ? (size_t)0 : n);
    if (w < 0) {
      return w;
    }
    c->out.tail += (size_t)w;
    produced += (size_t)w;
    fed = 1;
    if ((size_t)w < cap) {
      break;
    }
  }
  c->in.head = c->in.tail = 0;
  return dt_fifo_clamp_int(produced);
}

static int sdec_begin(dt_pipe *pipe) {
  sdec_pipe *c = (sdec_pipe *)pipe->data;
  size_t n = c->in.tail - c->in.head;
  int consumed = c->dec->begin(c->dec, c->in.buf + c->in.head, n);
  if (consumed < 0) {
    return consumed;
  }
  c->in.head += (size_t)consumed;
  if (c->in.head == c->in.tail) {
    c->in.head = c->in.tail = 0;
  }
  return 0;
}

static int sdec_tick(dt_pipe *pipe) { return sdec_run((sdec_pipe *)pipe->data); }

static int sdec_finalize(dt_pipe *pipe) {
  sdec_pipe *c = (sdec_pipe *)pipe->data;
  int a = sdec_run(c);
  if (a < 0) {
    return a;
  }
  if (dt_soft_fifo_reserve(&c->out, STREAM_EDGE_ROOM) < 0) {
    return -1;
  }
  int w = c->dec->finalize(c->dec, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
  if (w < 0) {
    return w;
  }
  c->out.tail += (size_t)w;
  return dt_fifo_clamp_int((size_t)a + (size_t)w);
}

static dt_pipe_source *sdec_get_source(dt_pipe *pipe) {
  return &((sdec_pipe *)pipe->data)->source;
}
static dt_pipe_sink *sdec_get_sink(dt_pipe *pipe) {
  return &((sdec_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_soft_decoder_create(dt_stream_soft_decoder *decoder) {
  if (!decoder) {
    return NULL;
  }
  sdec_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = sdec_get_source;
  c->base.sink = sdec_get_sink;
  c->base.begin = sdec_begin;
  c->base.tick = sdec_tick;
  c->base.finalize = sdec_finalize;
  c->base.data = c;
  c->source.pull = noop_pull;
  c->source.soft_pull = sdec_soft_pull;
  c->source.data = c;
  c->sink.push = sdec_push;
  c->sink.soft_push = noop_soft_push;
  c->sink.finish = noop_finish;
  c->sink.data = c;
  c->dec = decoder;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_soft_decoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  sdec_pipe *c = (sdec_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}
