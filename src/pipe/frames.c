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
 * Frame encoder / decoder / soft-decoder pipes: dt_pipe adapters over the frame
 * codecs (see frames.h). Like the streaming-codec adapters (streams.c) they buffer
 * input on the sink, run the wrapped codec in tick() over the whole buffered input,
 * and append its output to the buffer the source drains. The frame codecs add two
 * things the plain lifecycle cannot carry, handled here as extra calls:
 *
 *   - the encoder's begin_frame / end_frame, which emit the frame delimiters straight
 *     into the output buffer between ticks;
 *   - the decoder's get_state and advance - a frame decoder stops at each frame-state
 *     transition, so tick() drains straight through all of them while advance() takes
 *     a single step to the next one, letting the caller walk the frames.
 *
 * The wrapped codec is not owned. The dt_pipe is the first member, so destroy frees
 * the pipe through it.
 */

#include <drifty/pipe/frames.h>

#include <drifty/bit.h>
#include <drifty/frame_decoder.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include "fifo.h"

/* Room reserved for a codec's begin/finalize/delimiter output (preamble, trailer,
 * frame markers) - far above any drifty frame codec's need; grown if one ever reports
 * too little. */
#define FRAME_EDGE_ROOM 4096

/* -- shared no-op faces (an unused hard/soft face is callable, yields nothing) - */

static int noop_push(dt_pipe_sink *s, const dt_bit *x, size_t n) {
  (void)s; (void)x; (void)n; return 0;
}
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

/* -- frame encoder pipe (payload bits -> framed coded bits) ---------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_frame_encoder *enc; // not owned
  dt_bit_fifo in;        // buffered bits to encode
  dt_bit_fifo out;       // buffered framed output
} fenc_pipe;

static int fenc_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  return dt_bit_fifo_append(&((fenc_pipe *)sink->data)->in, src, src_len);
}
static int fenc_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  return dt_bit_fifo_drain(&((fenc_pipe *)source->data)->out, dst, dst_len);
}

/* Encode the whole input buffer into the output (grow and retry - encode checks room
 * up front and writes nothing when short). */
static int fenc_run(fenc_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  if (n == 0) {
    return 0;
  }
  size_t room = n * 2 + 64; /* framing/escaping can expand the stream */
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

/* Run a zero-input frame-encoder output op (begin / begin_frame / end_frame /
 * finalize) into the output buffer, growing until it fits. */
static int fenc_edge(fenc_pipe *c,
                     int (*op)(dt_frame_encoder *, dt_bit *, size_t)) {
  size_t room = FRAME_EDGE_ROOM;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, room) < 0) {
      return -1;
    }
    int w = op(c->enc, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
    if (w >= 0) {
      c->out.tail += (size_t)w;
      return dt_fifo_clamp_int((size_t)w);
    }
    room *= 2;
  }
}

static int fenc_begin(dt_pipe *pipe) {
  fenc_pipe *c = (fenc_pipe *)pipe->data;
  return fenc_edge(c, c->enc->begin) < 0 ? -1 : 0;
}
static int fenc_tick(dt_pipe *pipe) { return fenc_run((fenc_pipe *)pipe->data); }
static int fenc_finalize(dt_pipe *pipe) {
  fenc_pipe *c = (fenc_pipe *)pipe->data;
  int a = fenc_run(c);
  if (a < 0) {
    return a;
  }
  int b = fenc_edge(c, c->enc->finalize);
  if (b < 0) {
    return b;
  }
  return dt_fifo_clamp_int((size_t)a + (size_t)b);
}

static dt_pipe_source *fenc_get_source(dt_pipe *pipe) {
  return &((fenc_pipe *)pipe->data)->source;
}
static dt_pipe_sink *fenc_get_sink(dt_pipe *pipe) {
  return &((fenc_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_frame_encoder_create(dt_frame_encoder *encoder) {
  if (!encoder) {
    return NULL;
  }
  fenc_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = fenc_get_source;
  c->base.sink = fenc_get_sink;
  c->base.begin = fenc_begin;
  c->base.tick = fenc_tick;
  c->base.finalize = fenc_finalize;
  c->base.data = c;
  c->source.pull = fenc_pull;
  c->source.soft_pull = noop_soft_pull;
  c->source.data = c;
  c->sink.push = fenc_push;
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

void dt_pipe_frame_encoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  fenc_pipe *c = (fenc_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

int dt_pipe_frame_encoder_begin_frame(dt_pipe *pipe) {
  if (!pipe) {
    return -1;
  }
  fenc_pipe *c = (fenc_pipe *)pipe->data;
  return fenc_edge(c, c->enc->begin_frame);
}
int dt_pipe_frame_encoder_end_frame(dt_pipe *pipe) {
  if (!pipe) {
    return -1;
  }
  fenc_pipe *c = (fenc_pipe *)pipe->data;
  return fenc_edge(c, c->enc->end_frame);
}

/* -- frame decoder pipe (received bits -> recovered bits) ------------------ */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_frame_decoder *dec; // not owned
  dt_bit_fifo in;        // buffered received bits
  dt_bit_fifo out;       // buffered recovered bits
  int stalled;           // parked at a frame boundary until advance() releases it
} fdec_pipe;

static int fdec_push(dt_pipe_sink *sink, const dt_bit *src, size_t src_len) {
  return dt_bit_fifo_append(&((fdec_pipe *)sink->data)->in, src, src_len);
}
static int fdec_pull(dt_pipe_source *source, dt_bit *dst, size_t dst_len) {
  return dt_bit_fifo_drain(&((fdec_pipe *)source->data)->out, dst, dst_len);
}

/* One decode step: feed the buffered input (all of it, once) if any is waiting, else
 * drain with no input, growing dst so the call runs to the next frame-state change
 * rather than stopping on a full buffer. Emits the run before that change and returns
 * its length (0 at a zero-width transition or when dry). */
static int fdec_step(fdec_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  int fed = 0;
  size_t produced = 0;
  for (;;) {
    if (dt_bit_fifo_reserve(&c->out, 512) < 0) {
      return -1;
    }
    size_t cap = c->out.cap - c->out.tail;
    int feed = (!fed && n > 0);
    int w = c->dec->decode(c->dec, c->out.buf + c->out.tail, cap,
                           feed ? (c->in.buf + c->in.head) : NULL,
                           feed ? n : (size_t)0);
    if (w < 0) {
      return w;
    }
    c->out.tail += (size_t)w;
    produced += (size_t)w;
    if (feed) {
      c->in.head = c->in.tail = 0; /* decode() consumes all src, buffering internally */
      fed = 1;
    }
    if ((size_t)w == cap && cap > 0) {
      continue; /* filled dst mid-boundary: grow and keep going to the transition */
    }
    break; /* w < cap: reached a frame-state transition (or ran dry) */
  }
  return dt_fifo_clamp_int(produced);
}

/* Drain every remaining run straight through all frame boundaries (used by finalize,
 * which ignores the boundary stall). Stops on a step that emits nothing and does not
 * change state. */
static int fdec_drain(fdec_pipe *c) {
  size_t total = 0;
  dt_frame_decoder_state prev = c->dec->get_state(c->dec);
  for (;;) {
    int w = fdec_step(c);
    if (w < 0) {
      return w;
    }
    total += (size_t)w;
    dt_frame_decoder_state st = c->dec->get_state(c->dec);
    int moved = (w > 0) || (st != prev);
    prev = st;
    if (!moved) {
      break;
    }
  }
  return dt_fifo_clamp_int(total);
}

static int fdec_begin(dt_pipe *pipe) {
  fdec_pipe *c = (fdec_pipe *)pipe->data;
  c->stalled = 0;
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

/* tick: copy one run - decode up to the next frame-state change, then STALL there,
 * emitting nothing more until advance() is called. get_state() reports the boundary
 * reached; the bits this tick emitted are the run that preceded it. A dry step (no
 * bits, no state change) does not stall - more input can be pushed and ticked. */
static int fdec_tick(dt_pipe *pipe) {
  fdec_pipe *c = (fdec_pipe *)pipe->data;
  if (c->stalled) {
    return 0;
  }
  dt_frame_decoder_state prev = c->dec->get_state(c->dec);
  int w = fdec_step(c);
  if (w < 0) {
    return w;
  }
  if (c->dec->get_state(c->dec) != prev) {
    c->stalled = 1;
  }
  return w;
}

static int fdec_finalize(dt_pipe *pipe) {
  fdec_pipe *c = (fdec_pipe *)pipe->data;
  c->stalled = 0; /* flush past any boundary we were parked at */
  int a = fdec_drain(c);
  if (a < 0) {
    return a;
  }
  if (dt_bit_fifo_reserve(&c->out, FRAME_EDGE_ROOM) < 0) {
    return -1;
  }
  int w = c->dec->finalize(c->dec, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
  if (w < 0) {
    return w;
  }
  c->out.tail += (size_t)w;
  return dt_fifo_clamp_int((size_t)a + (size_t)w);
}

static dt_pipe_source *fdec_get_source(dt_pipe *pipe) {
  return &((fdec_pipe *)pipe->data)->source;
}
static dt_pipe_sink *fdec_get_sink(dt_pipe *pipe) {
  return &((fdec_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_frame_decoder_create(dt_frame_decoder *decoder) {
  if (!decoder) {
    return NULL;
  }
  fdec_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = fdec_get_source;
  c->base.sink = fdec_get_sink;
  c->base.begin = fdec_begin;
  c->base.tick = fdec_tick;
  c->base.finalize = fdec_finalize;
  c->base.data = c;
  c->source.pull = fdec_pull;
  c->source.soft_pull = noop_soft_pull;
  c->source.data = c;
  c->sink.push = fdec_push;
  c->sink.soft_push = noop_soft_push;
  c->sink.finish = noop_finish;
  c->sink.data = c;
  c->dec = decoder;
  c->stalled = 0;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_frame_decoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  fdec_pipe *c = (fdec_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

dt_frame_decoder_state dt_pipe_frame_decoder_get_state(dt_pipe *pipe) {
  if (!pipe) {
    return DT_FRAME_DECODER_OUTSIDE;
  }
  fdec_pipe *c = (fdec_pipe *)pipe->data;
  return c->dec->get_state(c->dec);
}

int dt_pipe_frame_decoder_advance(dt_pipe *pipe) {
  if (!pipe) {
    return 0;
  }
  ((fdec_pipe *)pipe->data)->stalled = 0; /* release: the next tick() copies past here */
  return 0;
}

/* -- frame soft-decoder pipe (received records -> recovered records) ------- */

typedef struct {
  dt_pipe base;
  dt_pipe_source source;
  dt_pipe_sink sink;
  dt_frame_soft_decoder *dec; // not owned
  dt_soft_fifo in;            // buffered received soft records
  dt_soft_fifo out;           // buffered recovered soft records
  int stalled;                // parked at a frame boundary until advance() releases it
} fsdec_pipe;

static int fsdec_soft_push(dt_pipe_sink *sink, const dt_soft_bit *src, size_t src_len) {
  return dt_soft_fifo_append(&((fsdec_pipe *)sink->data)->in, src, src_len);
}
static int fsdec_soft_pull(dt_pipe_source *source, dt_soft_bit *dst, size_t dst_len) {
  return dt_soft_fifo_drain(&((fsdec_pipe *)source->data)->out, dst, dst_len);
}

static int fsdec_step(fsdec_pipe *c) {
  size_t n = c->in.tail - c->in.head;
  int fed = 0;
  size_t produced = 0;
  for (;;) {
    if (dt_soft_fifo_reserve(&c->out, 512) < 0) {
      return -1;
    }
    size_t cap = c->out.cap - c->out.tail;
    int feed = (!fed && n > 0);
    int w = c->dec->decode(c->dec, c->out.buf + c->out.tail, cap,
                           feed ? (c->in.buf + c->in.head) : NULL,
                           feed ? n : (size_t)0);
    if (w < 0) {
      return w;
    }
    c->out.tail += (size_t)w;
    produced += (size_t)w;
    if (feed) {
      c->in.head = c->in.tail = 0;
      fed = 1;
    }
    if ((size_t)w == cap && cap > 0) {
      continue;
    }
    break;
  }
  return dt_fifo_clamp_int(produced);
}

static int fsdec_drain(fsdec_pipe *c) {
  size_t total = 0;
  dt_frame_decoder_state prev = c->dec->get_state(c->dec);
  for (;;) {
    int w = fsdec_step(c);
    if (w < 0) {
      return w;
    }
    total += (size_t)w;
    dt_frame_decoder_state st = c->dec->get_state(c->dec);
    int moved = (w > 0) || (st != prev);
    prev = st;
    if (!moved) {
      break;
    }
  }
  return dt_fifo_clamp_int(total);
}

static int fsdec_begin(dt_pipe *pipe) {
  fsdec_pipe *c = (fsdec_pipe *)pipe->data;
  c->stalled = 0;
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

/* tick: copy one run up to the next frame-state change, then stall (see fdec_tick). */
static int fsdec_tick(dt_pipe *pipe) {
  fsdec_pipe *c = (fsdec_pipe *)pipe->data;
  if (c->stalled) {
    return 0;
  }
  dt_frame_decoder_state prev = c->dec->get_state(c->dec);
  int w = fsdec_step(c);
  if (w < 0) {
    return w;
  }
  if (c->dec->get_state(c->dec) != prev) {
    c->stalled = 1;
  }
  return w;
}

static int fsdec_finalize(dt_pipe *pipe) {
  fsdec_pipe *c = (fsdec_pipe *)pipe->data;
  c->stalled = 0;
  int a = fsdec_drain(c);
  if (a < 0) {
    return a;
  }
  if (dt_soft_fifo_reserve(&c->out, FRAME_EDGE_ROOM) < 0) {
    return -1;
  }
  int w = c->dec->finalize(c->dec, c->out.buf + c->out.tail, c->out.cap - c->out.tail);
  if (w < 0) {
    return w;
  }
  c->out.tail += (size_t)w;
  return dt_fifo_clamp_int((size_t)a + (size_t)w);
}

static dt_pipe_source *fsdec_get_source(dt_pipe *pipe) {
  return &((fsdec_pipe *)pipe->data)->source;
}
static dt_pipe_sink *fsdec_get_sink(dt_pipe *pipe) {
  return &((fsdec_pipe *)pipe->data)->sink;
}

dt_pipe *dt_pipe_frame_soft_decoder_create(dt_frame_soft_decoder *decoder) {
  if (!decoder) {
    return NULL;
  }
  fsdec_pipe *c = dt_malloc(sizeof(*c));
  if (!c) {
    return NULL;
  }
  c->base.source = fsdec_get_source;
  c->base.sink = fsdec_get_sink;
  c->base.begin = fsdec_begin;
  c->base.tick = fsdec_tick;
  c->base.finalize = fsdec_finalize;
  c->base.data = c;
  c->source.pull = noop_pull;
  c->source.soft_pull = fsdec_soft_pull;
  c->source.data = c;
  c->sink.push = noop_push;
  c->sink.soft_push = fsdec_soft_push;
  c->sink.finish = noop_finish;
  c->sink.data = c;
  c->dec = decoder;
  c->stalled = 0;
  c->in.buf = NULL;
  c->in.cap = c->in.head = c->in.tail = 0;
  c->out.buf = NULL;
  c->out.cap = c->out.head = c->out.tail = 0;
  return &c->base;
}

void dt_pipe_frame_soft_decoder_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  fsdec_pipe *c = (fsdec_pipe *)pipe;
  dt_free(c->in.buf);
  dt_free(c->out.buf);
  dt_free(c);
}

dt_frame_decoder_state dt_pipe_frame_soft_decoder_get_state(dt_pipe *pipe) {
  if (!pipe) {
    return DT_FRAME_DECODER_OUTSIDE;
  }
  fsdec_pipe *c = (fsdec_pipe *)pipe->data;
  return c->dec->get_state(c->dec);
}

int dt_pipe_frame_soft_decoder_advance(dt_pipe *pipe) {
  if (!pipe) {
    return 0;
  }
  ((fsdec_pipe *)pipe->data)->stalled = 0; /* release: the next tick() copies past here */
  return 0;
}
