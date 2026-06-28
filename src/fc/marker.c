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
 * marker frame codec - variable-length frames delimited by 18-bit escape
 * sequences (fifteen 1s + a 3-bit code). See <drifty/fc/marker.h> for the wire
 * format. The stream is kept transparent by escaping any payload run that would
 * form a sequence: a run is emitted as a pure escape (1^15 000) plus a 2-bit
 * suffix naming which sequence the payload held.
 *
 * Both the encoder and the two decoders are bit-level state machines that buffer
 * their output internally (a token FIFO) - escaping changes the bit count, so
 * unlike `naive` they do not pass through 1:1. Drive them with the standard frame
 * buffering rule: a call that returns exactly dst_len has more buffered, so call
 * again with no new input (src_len 0) to drain.
 *
 * The suffix-00 (pure) escape reconstructs only the fifteen 1s and lets the code
 * bits ride through as ordinary data; suffixes 01/10/11 reconstruct the whole
 * 18-bit sequence. Reconstructing just the 1s is what lets a payload that ends a
 * frame on a bare prefix (1^15 0) be escaped cleanly even though it is not a full
 * sequence - the encoder breaks the run before the following marker.
 */

#include <drifty/fc/marker.h>

#include <drifty/bit.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h> /* dt_malloc / dt_free / dt_realloc / dt_memcpy / dt_memmove / dt_memset */

#define MARK_RUN 15 /* number of 1s in an escape-sequence prefix */

/* A received/transmit symbol's role in framing. */
enum { CL_ONE, CL_ZERO, CL_OTHER };

static int classify_hard(dt_bit b) {
  if (b == DT_TRUE) {
    return CL_ONE;
  }
  if (b == DT_FALSE) {
    return CL_ZERO;
  }
  return CL_OTHER;
}

/* -- generic growable element FIFO ----------------------------------------- */

typedef struct {
  unsigned char *d;
  size_t cap, head, tail; /* in elements */
  size_t esz;             /* element size in bytes */
} fifo;

static void fifo_init(fifo *f, size_t esz) {
  f->d = NULL;
  f->cap = f->head = f->tail = 0;
  f->esz = esz;
}

static void fifo_free(fifo *f) {
  dt_free(f->d);
  f->d = NULL;
  f->cap = f->head = f->tail = 0;
}

static int fifo_push(fifo *f, const void *e) {
  if (f->tail == f->cap) {
    if (f->head > 0) { /* compact to the front */
      dt_memmove(f->d, f->d + f->head * f->esz, (f->tail - f->head) * f->esz);
      f->tail -= f->head;
      f->head = 0;
    }
    if (f->tail == f->cap) {
      size_t nc = f->cap ? f->cap * 2 : 64;
      void *nd = dt_realloc(f->d, nc * f->esz);
      if (!nd) {
        return 0;
      }
      f->d = nd;
      f->cap = nc;
    }
  }
  dt_memcpy(f->d + f->tail * f->esz, e, f->esz);
  f->tail++;
  return 1;
}

static const void *fifo_peek(const fifo *f) {
  return f->head < f->tail ? f->d + f->head * f->esz : NULL;
}

static void fifo_pop(fifo *f) {
  f->head++;
  if (f->head == f->tail) {
    f->head = f->tail = 0;
  }
}

/* -- encoder --------------------------------------------------------------- */

typedef struct {
  int in_frame;   /* 0 outside a frame, 1 inside */
  size_t p;       /* held leading 1s, 0..MARK_RUN */
  int esc;        /* 0 none, 1 saw trigger 0, 2 saw 0 + one code bit */
  dt_bit ecode1;  /* the second code bit, when esc == 2 */
  fifo out;       /* pending output bits (dt_bit) */
} marker_encoder;

static void e_bit(marker_encoder *st, dt_bit v) { fifo_push(&st->out, &v); }

static void e_ones(marker_encoder *st, size_t n) {
  dt_bit one = DT_TRUE;
  for (size_t i = 0; i < n; ++i) {
    fifo_push(&st->out, &one);
  }
}

/* Emit a pure escape (1^15 000) + 2-bit suffix (MSB first). */
static void e_escape(marker_encoder *st, int suffix) {
  e_ones(st, MARK_RUN);
  e_bit(st, DT_FALSE);
  e_bit(st, DT_FALSE);
  e_bit(st, DT_FALSE);
  e_bit(st, (suffix & 2) ? DT_TRUE : DT_FALSE);
  e_bit(st, (suffix & 1) ? DT_TRUE : DT_FALSE);
}

/* Emit a real marker 1^15 + 3-bit code (MSB first). */
static void e_marker(marker_encoder *st, int code) {
  e_ones(st, MARK_RUN);
  e_bit(st, (code & 4) ? DT_TRUE : DT_FALSE);
  e_bit(st, (code & 2) ? DT_TRUE : DT_FALSE);
  e_bit(st, (code & 1) ? DT_TRUE : DT_FALSE);
}

static void e_process(marker_encoder *st, dt_bit b);

/* Represent the MARK_RUN held 1s as a pure escape (suffix 00). */
static void e_run_escape(marker_encoder *st) {
  e_escape(st, 0);
  st->p = 0;
}

static void e_process(marker_encoder *st, dt_bit b) {
  if (st->esc == 0) {
    int cl = classify_hard(b);
    if (cl == CL_ONE) {
      if (st->p < MARK_RUN) {
        st->p++;
      } else {
        e_bit(st, DT_TRUE); /* run already 15: oldest 1 is data (decoder peels) */
      }
    } else if (cl == CL_ZERO && st->p == MARK_RUN) {
      st->esc = 1; /* 1^15 0 - a sequence may start; gather the code */
    } else {
      e_ones(st, st->p); /* flush held 1s, then this symbol verbatim */
      st->p = 0;
      e_bit(st, b);
    }
  } else if (st->esc == 1) {
    int cl = classify_hard(b);
    if (cl == CL_ONE || cl == CL_ZERO) {
      st->ecode1 = b;
      st->esc = 2;
    } else { /* not a clean code bit: break the run, replay 0 then b */
      e_run_escape(st);
      st->esc = 0;
      e_process(st, DT_FALSE);
      e_process(st, b);
    }
  } else { /* esc == 2: have 0 + ecode1, b is the last code bit */
    int cl = classify_hard(b);
    if (cl == CL_ONE || cl == CL_ZERO) {
      int code = ((st->ecode1 == DT_TRUE) << 1) | (b == DT_TRUE);
      st->esc = 0;
      if (code == 0) { /* pure 000: break the run, let 000 ride as raw data */
        dt_bit c1 = st->ecode1;
        e_run_escape(st);
        e_process(st, DT_FALSE);
        e_process(st, c1);
        e_process(st, b);
      } else { /* 001/010/011 carried in the suffix */
        e_escape(st, code);
        st->p = 0;
      }
    } else {
      dt_bit c1 = st->ecode1;
      e_run_escape(st);
      st->esc = 0;
      e_process(st, DT_FALSE);
      e_process(st, c1);
      e_process(st, b);
    }
  }
}

/* Resolve a half-gathered sequence and flush held 1s ahead of a marker/EOF. */
static void e_flush(marker_encoder *st) {
  if (st->esc == 1) {
    e_run_escape(st);
    st->esc = 0;
    e_process(st, DT_FALSE);
  } else if (st->esc == 2) {
    dt_bit c1 = st->ecode1;
    e_run_escape(st);
    st->esc = 0;
    e_process(st, DT_FALSE);
    e_process(st, c1);
  }
  e_ones(st, st->p); /* remaining held 1s are data; a marker's 1s extend the run */
  st->p = 0;
}

static int e_drain(marker_encoder *st, dt_bit *dst, size_t dst_len) {
  size_t w = 0;
  const void *e;
  while (w < dst_len && (e = fifo_peek(&st->out)) != NULL) {
    dst[w++] = *(const dt_bit *)e;
    fifo_pop(&st->out);
  }
  return (int)w;
}

static int me_begin(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len) {
  (void)dst;
  (void)dst_len;
  (void)enc;
  return 0;
}

static int me_begin_frame(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len) {
  marker_encoder *st = enc->data;
  e_flush(st);
  e_marker(st, st->in_frame ? 3 : 1); /* 011 from inside, else 001 from outside */
  st->in_frame = 1;
  return e_drain(st, dst, dst_len);
}

static int me_encode(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len,
                     const dt_bit *src, size_t src_len) {
  marker_encoder *st = enc->data;
  for (size_t i = 0; i < src_len; ++i) {
    e_process(st, src[i]);
  }
  return e_drain(st, dst, dst_len);
}

static int me_end_frame(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len) {
  marker_encoder *st = enc->data;
  e_flush(st);
  e_marker(st, 2); /* 010 end */
  st->in_frame = 0;
  return e_drain(st, dst, dst_len);
}

static int me_finalize(dt_frame_encoder *enc, dt_bit *dst, size_t dst_len) {
  marker_encoder *st = enc->data;
  e_flush(st);
  return e_drain(st, dst, dst_len);
}

dt_frame_encoder *dt_fc_marker_frame_encoder_create(void) {
  dt_frame_encoder *enc = dt_malloc(sizeof(*enc));
  marker_encoder *st = dt_malloc(sizeof(*st));
  if (!enc || !st) {
    dt_free(enc);
    dt_free(st);
    return NULL;
  }
  st->in_frame = 0;
  st->p = 0;
  st->esc = 0;
  st->ecode1 = DT_FALSE;
  fifo_init(&st->out, sizeof(dt_bit));
  enc->begin = me_begin;
  enc->begin_frame = me_begin_frame;
  enc->encode = me_encode;
  enc->end_frame = me_end_frame;
  enc->finalize = me_finalize;
  enc->data = st;
  return enc;
}

void dt_fc_marker_frame_encoder_destroy(dt_frame_encoder *enc) {
  if (!enc) {
    return;
  }
  fifo_free(&((marker_encoder *)enc->data)->out);
  dt_free(enc->data);
  dt_free(enc);
}

/* -- decoder bit-machine (shared structure) -------------------------------- */

enum { BM_SCAN, BM_CODE2, BM_CODE3, BM_SUF1, BM_SUF2 };

/* -- hard decoder ---------------------------------------------------------- */

typedef struct {
  unsigned char kind; /* 0 = data bit, 1 = state transition */
  unsigned char v;    /* dt_bit, or dt_frame_decoder_state */
} htok;

typedef struct {
  size_t t;   /* held trailing 1s, 0..MARK_RUN */
  int bm;     /* BM_* */
  int code;   /* assembled 3-bit code */
  int suf;    /* first suffix bit */
  dt_frame_decoder_state fstate;
  fifo out;   /* htok */
} marker_decoder;

static void d_data(marker_decoder *st, dt_bit b) {
  htok k = {0, (unsigned char)b};
  fifo_push(&st->out, &k);
}

static void d_state(marker_decoder *st, dt_frame_decoder_state s) {
  htok k = {1, (unsigned char)s};
  fifo_push(&st->out, &k);
}

static void d_ones(marker_decoder *st, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    d_data(st, DT_TRUE);
  }
}

static void d_marker(marker_decoder *st, int code) {
  if (code == 1) { /* 001 begin from outside */
    d_state(st, DT_FRAME_DECODER_BEGIN);
    d_state(st, DT_FRAME_DECODER_INSIDE);
  } else if (code == 2) { /* 010 end */
    d_state(st, DT_FRAME_DECODER_END);
    d_state(st, DT_FRAME_DECODER_OUTSIDE);
  } else { /* 011 begin from inside */
    d_state(st, DT_FRAME_DECODER_END);
    d_state(st, DT_FRAME_DECODER_BEGIN);
    d_state(st, DT_FRAME_DECODER_INSIDE);
  }
}

static void d_feed(marker_decoder *st, int cl, dt_bit raw) {
  switch (st->bm) {
  case BM_SCAN:
    if (cl == CL_ONE) {
      if (st->t < MARK_RUN) {
        st->t++;
      } else {
        d_data(st, DT_TRUE); /* peel: excess of a long run is data */
      }
    } else if (cl == CL_ZERO) {
      if (st->t == MARK_RUN) {
        st->code = 0; /* 1^15 0: a sequence starts; the 15 held 1s are consumed */
        st->bm = BM_CODE2;
      } else {
        d_ones(st, st->t);
        d_data(st, DT_FALSE);
        st->t = 0;
      }
    } else {
      d_ones(st, st->t);
      d_data(st, raw);
      st->t = 0;
    }
    break;
  case BM_CODE2:
    st->code = (st->code << 1) | (cl == CL_ONE ? 1 : 0);
    st->bm = BM_CODE3;
    break;
  case BM_CODE3:
    st->code = (st->code << 1) | (cl == CL_ONE ? 1 : 0);
    if (st->code == 0) {
      st->bm = BM_SUF1; /* pure escape: read the 2-bit suffix */
    } else {
      d_marker(st, st->code);
      st->t = 0;
      st->bm = BM_SCAN;
    }
    break;
  case BM_SUF1:
    st->suf = (cl == CL_ONE ? 1 : 0);
    st->bm = BM_SUF2;
    break;
  case BM_SUF2: {
    int sfx = (st->suf << 1) | (cl == CL_ONE ? 1 : 0);
    if (sfx == 0) {
      d_ones(st, MARK_RUN); /* the 000 code rides through as raw data */
    } else {
      d_ones(st, MARK_RUN);
      d_data(st, DT_FALSE);
      d_data(st, (sfx & 2) ? DT_TRUE : DT_FALSE);
      d_data(st, (sfx & 1) ? DT_TRUE : DT_FALSE);
    }
    st->t = 0;
    st->bm = BM_SCAN;
    break;
  }
  }
}

static int d_drain(marker_decoder *st, dt_bit *dst, size_t dst_len) {
  size_t w = 0;
  const htok *k;
  while ((k = fifo_peek(&st->out)) != NULL) {
    if (k->kind == 1) {
      st->fstate = (dt_frame_decoder_state)k->v;
      fifo_pop(&st->out);
      return (int)w; /* stop at the transition */
    }
    if (w == dst_len) {
      return (int)w; /* dst full */
    }
    dst[w++] = (dt_bit)k->v;
    fifo_pop(&st->out);
  }
  return (int)w;
}

static int md_begin(dt_frame_decoder *dec, const dt_bit *src, size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0;
}

static dt_frame_decoder_state md_get_state(dt_frame_decoder *dec) {
  return ((marker_decoder *)dec->data)->fstate;
}

static int md_decode(dt_frame_decoder *dec, dt_bit *dst, size_t dst_len,
                     const dt_bit *src, size_t src_len) {
  marker_decoder *st = dec->data;
  for (size_t i = 0; i < src_len; ++i) {
    d_feed(st, classify_hard(src[i]), src[i]);
  }
  return d_drain(st, dst, dst_len);
}

static int md_finalize(dt_frame_decoder *dec, dt_bit *dst, size_t dst_len) {
  marker_decoder *st = dec->data;
  if (st->bm == BM_SCAN) {
    d_ones(st, st->t);
  } else { /* truncated mid-sequence: recover the consumed prefix best-effort */
    d_ones(st, MARK_RUN);
    if (st->bm == BM_CODE3 || st->bm == BM_SUF1 || st->bm == BM_SUF2) {
      d_data(st, DT_FALSE);
    }
    st->bm = BM_SCAN;
  }
  st->t = 0;
  return d_drain(st, dst, dst_len);
}

dt_frame_decoder *dt_fc_marker_frame_decoder_create(void) {
  dt_frame_decoder *dec = dt_malloc(sizeof(*dec));
  marker_decoder *st = dt_malloc(sizeof(*st));
  if (!dec || !st) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->t = 0;
  st->bm = BM_SCAN;
  st->code = 0;
  st->suf = 0;
  st->fstate = DT_FRAME_DECODER_OUTSIDE;
  fifo_init(&st->out, sizeof(htok));
  dec->begin = md_begin;
  dec->get_state = md_get_state;
  dec->decode = md_decode;
  dec->finalize = md_finalize;
  dec->data = st;
  return dec;
}

void dt_fc_marker_frame_decoder_destroy(dt_frame_decoder *dec) {
  if (!dec) {
    return;
  }
  fifo_free(&((marker_decoder *)dec->data)->out);
  dt_free(dec->data);
  dt_free(dec);
}

/* -- soft decoder ---------------------------------------------------------- */

static dt_soft_bit soft_one(void) {
  dt_soft_bit s;
  dt_memset(&s, 0, sizeof s);
  s.c_true = 1.0f;
  s.c_locked = 1.0f;
  return s;
}

static dt_soft_bit soft_zero(void) {
  dt_soft_bit s;
  dt_memset(&s, 0, sizeof s);
  s.c_false = 1.0f;
  s.c_locked = 1.0f;
  return s;
}

static int classify_soft(dt_soft_bit s) {
  float v[5] = {s.c_false, s.c_true, s.c_erasure, s.c_invalid, s.c_absent};
  int mi = 0;
  for (int i = 1; i < 5; ++i) {
    if (v[i] > v[mi]) {
      mi = i;
    }
  }
  if (mi == 1) {
    return CL_ONE;
  }
  if (mi == 0) {
    return CL_ZERO;
  }
  return CL_OTHER;
}

typedef struct {
  unsigned char kind;  /* 0 = data record, 1 = state transition */
  unsigned char state; /* dt_frame_decoder_state, when kind == 1 */
  dt_soft_bit s;       /* data record, when kind == 0 */
} stok;

typedef struct {
  dt_soft_bit hold[MARK_RUN]; /* held trailing 1s, originals (for pass-through) */
  size_t t;                   /* count held, 0..MARK_RUN */
  int bm;
  int code;
  int suf;
  dt_frame_decoder_state fstate;
  fifo out; /* stok */
} marker_soft_decoder;

static void sd_data(marker_soft_decoder *st, dt_soft_bit s) {
  stok k;
  k.kind = 0;
  k.state = 0;
  k.s = s;
  fifo_push(&st->out, &k);
}

static void sd_state(marker_soft_decoder *st, dt_frame_decoder_state s) {
  stok k;
  k.kind = 1;
  k.state = (unsigned char)s;
  dt_memset(&k.s, 0, sizeof k.s);
  fifo_push(&st->out, &k);
}

static void sd_emit_held(marker_soft_decoder *st) {
  for (size_t i = 0; i < st->t; ++i) {
    sd_data(st, st->hold[i]);
  }
  st->t = 0;
}

static void sd_ones(marker_soft_decoder *st, size_t n) {
  dt_soft_bit one = soft_one();
  for (size_t i = 0; i < n; ++i) {
    sd_data(st, one);
  }
}

static void sd_marker(marker_soft_decoder *st, int code) {
  if (code == 1) {
    sd_state(st, DT_FRAME_DECODER_BEGIN);
    sd_state(st, DT_FRAME_DECODER_INSIDE);
  } else if (code == 2) {
    sd_state(st, DT_FRAME_DECODER_END);
    sd_state(st, DT_FRAME_DECODER_OUTSIDE);
  } else {
    sd_state(st, DT_FRAME_DECODER_END);
    sd_state(st, DT_FRAME_DECODER_BEGIN);
    sd_state(st, DT_FRAME_DECODER_INSIDE);
  }
}

static void sd_feed(marker_soft_decoder *st, int cl, dt_soft_bit s) {
  switch (st->bm) {
  case BM_SCAN:
    if (cl == CL_ONE) {
      if (st->t < MARK_RUN) {
        st->hold[st->t++] = s;
      } else { /* peel oldest held 1 as data, shift the window */
        sd_data(st, st->hold[0]);
        dt_memmove(st->hold, st->hold + 1, (MARK_RUN - 1) * sizeof(dt_soft_bit));
        st->hold[MARK_RUN - 1] = s;
      }
    } else if (cl == CL_ZERO) {
      if (st->t == MARK_RUN) {
        st->code = 0; /* sequence: the 15 held 1s are consumed */
        st->bm = BM_CODE2;
      } else {
        sd_emit_held(st);
        sd_data(st, s);
      }
    } else {
      sd_emit_held(st);
      sd_data(st, s);
    }
    break;
  case BM_CODE2:
    st->code = (st->code << 1) | (cl == CL_ONE ? 1 : 0);
    st->bm = BM_CODE3;
    break;
  case BM_CODE3:
    st->code = (st->code << 1) | (cl == CL_ONE ? 1 : 0);
    if (st->code == 0) {
      st->bm = BM_SUF1;
    } else {
      sd_marker(st, st->code);
      st->t = 0;
      st->bm = BM_SCAN;
    }
    break;
  case BM_SUF1:
    st->suf = (cl == CL_ONE ? 1 : 0);
    st->bm = BM_SUF2;
    break;
  case BM_SUF2: {
    int sfx = (st->suf << 1) | (cl == CL_ONE ? 1 : 0);
    if (sfx == 0) {
      sd_ones(st, MARK_RUN);
    } else {
      sd_ones(st, MARK_RUN);
      sd_data(st, soft_zero());
      sd_data(st, (sfx & 2) ? soft_one() : soft_zero());
      sd_data(st, (sfx & 1) ? soft_one() : soft_zero());
    }
    st->t = 0;
    st->bm = BM_SCAN;
    break;
  }
  }
}

static int sd_drain(marker_soft_decoder *st, dt_soft_bit *dst, size_t dst_len) {
  size_t w = 0;
  const stok *k;
  while ((k = fifo_peek(&st->out)) != NULL) {
    if (k->kind == 1) {
      st->fstate = (dt_frame_decoder_state)k->state;
      fifo_pop(&st->out);
      return (int)w;
    }
    if (w == dst_len) {
      return (int)w;
    }
    dst[w++] = k->s;
    fifo_pop(&st->out);
  }
  return (int)w;
}

static int msd_begin(dt_frame_soft_decoder *dec, const dt_soft_bit *src,
                     size_t src_len) {
  (void)dec;
  (void)src;
  (void)src_len;
  return 0;
}

static dt_frame_decoder_state msd_get_state(dt_frame_soft_decoder *dec) {
  return ((marker_soft_decoder *)dec->data)->fstate;
}

static int msd_decode(dt_frame_soft_decoder *dec, dt_soft_bit *dst,
                      size_t dst_len, const dt_soft_bit *src, size_t src_len) {
  marker_soft_decoder *st = dec->data;
  for (size_t i = 0; i < src_len; ++i) {
    sd_feed(st, classify_soft(src[i]), src[i]);
  }
  return sd_drain(st, dst, dst_len);
}

static int msd_finalize(dt_frame_soft_decoder *dec, dt_soft_bit *dst,
                        size_t dst_len) {
  marker_soft_decoder *st = dec->data;
  if (st->bm == BM_SCAN) {
    sd_emit_held(st);
  } else {
    sd_ones(st, MARK_RUN);
    if (st->bm == BM_CODE3 || st->bm == BM_SUF1 || st->bm == BM_SUF2) {
      sd_data(st, soft_zero());
    }
    st->bm = BM_SCAN;
    st->t = 0;
  }
  return sd_drain(st, dst, dst_len);
}

dt_frame_soft_decoder *dt_fc_marker_frame_soft_decoder_create(void) {
  dt_frame_soft_decoder *dec = dt_malloc(sizeof(*dec));
  marker_soft_decoder *st = dt_malloc(sizeof(*st));
  if (!dec || !st) {
    dt_free(dec);
    dt_free(st);
    return NULL;
  }
  st->t = 0;
  st->bm = BM_SCAN;
  st->code = 0;
  st->suf = 0;
  st->fstate = DT_FRAME_DECODER_OUTSIDE;
  fifo_init(&st->out, sizeof(stok));
  dec->begin = msd_begin;
  dec->get_state = msd_get_state;
  dec->decode = msd_decode;
  dec->finalize = msd_finalize;
  dec->data = st;
  return dec;
}

void dt_fc_marker_frame_soft_decoder_destroy(dt_frame_soft_decoder *dec) {
  if (!dec) {
    return;
  }
  fifo_free(&((marker_soft_decoder *)dec->data)->out);
  dt_free(dec->data);
  dt_free(dec);
}
