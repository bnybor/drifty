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
 * dt_pipe_splitter: a tee (see multi.h). It embeds the two-ended buffered core
 * (bufcore.h) and an array of extra sinks. Each of begin / tick / finalize drains
 * the buffered input and copies every element both to the output buffer (which
 * the pipe's own source drains) and to each extra sink. The dt_pipe handle is the
 * first member, so the factory returns &self->base and destroy frees through it.
 */

#include <drifty/pipe/multi.h>

#include <drifty/bit.h>
#include <drifty/pipe/pipe.h>
#include <drifty/pipe/pipes.h> /* dt_pipe_pump */
#include <drifty/pipe/sink.h>
#include <drifty/pipe/source.h>
#include <drifty/soft_bit.h>
#include <drifty/stdlib.h>

#include "bufcore.h"

#define MULTI_CHUNK 256

typedef struct {
  dt_pipe base;
  dt_pipe_ends ends;
  dt_pipe_sink **sinks; // owned array copy; the sinks are not owned
  size_t n;
} splitter;

/* Push a whole hard/soft chunk to one sink (looping on short pushes). Returns n,
 * or a negative value on error (the sink refuses data with room remaining). */
static int split_push_hard(dt_pipe_sink *k, const dt_bit *b, int n) {
  int off = 0;
  while (off < n) {
    int w = k->push(k, b + off, (size_t)(n - off));
    if (w < 0) {
      return w;
    }
    if (w == 0) {
      return -1;
    }
    off += w;
  }
  return n;
}
static int split_push_soft(dt_pipe_sink *k, const dt_soft_bit *b, int n) {
  int off = 0;
  while (off < n) {
    int w = k->soft_push(k, b + off, (size_t)(n - off));
    if (w < 0) {
      return w;
    }
    if (w == 0) {
      return -1;
    }
    off += w;
  }
  return n;
}

/* The operation: drain the input buffer, copying each chunk to the output buffer
 * and to every extra sink (skipping any that lacks the carried face). */
static int splitter_copy(splitter *s) {
  dt_pipe_source *in = &s->ends.in_src;
  dt_pipe_sink *out = &s->ends.out_snk;
  int total = 0;
  for (;;) {
    dt_bit b[MULTI_CHUNK];
    int g = in->pull(in, b, MULTI_CHUNK);
    if (g < 0) {
      return g;
    }
    if (g == 0) {
      break;
    }
    int r = split_push_hard(out, b, g);
    if (r < 0) {
      return r;
    }
    for (size_t i = 0; i < s->n; ++i) {
      if (s->sinks[i]->push) {
        r = split_push_hard(s->sinks[i], b, g);
        if (r < 0) {
          return r;
        }
      }
    }
    total += g;
  }
  for (;;) {
    dt_soft_bit b[MULTI_CHUNK];
    int g = in->soft_pull(in, b, MULTI_CHUNK);
    if (g < 0) {
      return g;
    }
    if (g == 0) {
      break;
    }
    int r = split_push_soft(out, b, g);
    if (r < 0) {
      return r;
    }
    for (size_t i = 0; i < s->n; ++i) {
      if (s->sinks[i]->soft_push) {
        r = split_push_soft(s->sinks[i], b, g);
        if (r < 0) {
          return r;
        }
      }
    }
    total += g;
  }
  return total;
}

static int splitter_begin(dt_pipe *pipe) { return splitter_copy((splitter *)pipe->data); }
static int splitter_tick(dt_pipe *pipe) { return splitter_copy((splitter *)pipe->data); }
static int splitter_finalize(dt_pipe *pipe) { return splitter_copy((splitter *)pipe->data); }

static dt_pipe_source *splitter_get_source(dt_pipe *pipe) {
  return &((splitter *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *splitter_get_sink(dt_pipe *pipe) {
  return &((splitter *)pipe->data)->ends.pub_sink;
}

dt_pipe *dt_pipe_splitter_create(dt_pipe_sink **sinks, size_t count) {
  if (count > 0 && !sinks) {
    return NULL;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!sinks[i]) {
      return NULL;
    }
  }
  splitter *s = dt_malloc(sizeof(*s));
  if (!s) {
    return NULL;
  }
  if (count > 0) {
    s->sinks = dt_malloc(count * sizeof(*s->sinks));
    if (!s->sinks) {
      dt_free(s);
      return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
      s->sinks[i] = sinks[i];
    }
  } else {
    s->sinks = NULL;
  }
  s->n = count;
  s->base.source = splitter_get_source;
  s->base.sink = splitter_get_sink;
  s->base.begin = splitter_begin;
  s->base.tick = splitter_tick;
  s->base.finalize = splitter_finalize;
  s->base.data = s;
  dt_pipe_ends_init(&s->ends);
  return &s->base;
}

void dt_pipe_splitter_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  splitter *s = (splitter *)pipe; // base is the first member
  dt_pipe_ends_free(&s->ends);
  dt_free(s->sinks);
  dt_free(s);
}

/* -- diverter: copy the input to one selected output ----------------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_ends ends;
  dt_pipe_sink **sinks; // owned array copy; the sinks are not owned
  size_t n;
  size_t selected; // 0 = the output buffer; k = sinks[k-1]
} diverter;

/* The operation: pump the buffered input to the current selection - the output
 * buffer (0), the chosen sink (1..n), or nowhere (a selection past n drops it,
 * since dt_pipe_pump with a NULL sink drains and discards). */
static int diverter_copy(diverter *d) {
  dt_pipe_sink *target;
  if (d->selected == 0) {
    target = &d->ends.out_snk;
  } else if (d->selected - 1 < d->n) {
    target = d->sinks[d->selected - 1];
  } else {
    target = NULL; /* selection past the sinks: drop the input */
  }
  return dt_pipe_pump(&d->ends.in_src, target);
}

static int diverter_begin(dt_pipe *pipe) { return diverter_copy((diverter *)pipe->data); }
static int diverter_tick(dt_pipe *pipe) { return diverter_copy((diverter *)pipe->data); }
static int diverter_finalize(dt_pipe *pipe) { return diverter_copy((diverter *)pipe->data); }

static dt_pipe_source *diverter_get_source(dt_pipe *pipe) {
  return &((diverter *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *diverter_get_sink(dt_pipe *pipe) {
  return &((diverter *)pipe->data)->ends.pub_sink;
}

dt_pipe *dt_pipe_diverter_create(dt_pipe_sink **sinks, size_t len) {
  if (len > 0 && !sinks) {
    return NULL;
  }
  for (size_t i = 0; i < len; ++i) {
    if (!sinks[i]) {
      return NULL;
    }
  }
  diverter *d = dt_malloc(sizeof(*d));
  if (!d) {
    return NULL;
  }
  if (len > 0) {
    d->sinks = dt_malloc(len * sizeof(*d->sinks));
    if (!d->sinks) {
      dt_free(d);
      return NULL;
    }
    for (size_t i = 0; i < len; ++i) {
      d->sinks[i] = sinks[i];
    }
  } else {
    d->sinks = NULL;
  }
  d->n = len;
  d->selected = 0;
  d->base.source = diverter_get_source;
  d->base.sink = diverter_get_sink;
  d->base.begin = diverter_begin;
  d->base.tick = diverter_tick;
  d->base.finalize = diverter_finalize;
  d->base.data = d;
  dt_pipe_ends_init(&d->ends);
  return &d->base;
}

void dt_pipe_diverter_select(dt_pipe *pipe, size_t idx) {
  ((diverter *)pipe->data)->selected = idx;
}

void dt_pipe_diverter_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  diverter *d = (diverter *)pipe; // base is the first member
  dt_pipe_ends_free(&d->ends);
  dt_free(d->sinks);
  dt_free(d);
}

/* -- selector: pump one selected input to the output ----------------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_ends ends;
  dt_pipe_source **sources; // owned array copy; the sources are not owned
  size_t n;
  size_t selected; // 0 = the input buffer; k = sources[k-1]
} selector;

/* The operation: pump the selected input to the output buffer and every other
 * input (the own buffer and the rest of the sources) to NULL - draining and
 * discarding it. The inputs are indexed 0 (own buffer) then 1..n (sources). */
static int selector_run(selector *s) {
  int moved = 0;
  int r;
  if (s->selected == 0) {
    r = dt_pipe_pump(&s->ends.in_src, &s->ends.out_snk);
    if (r < 0) {
      return r;
    }
    moved += r;
  } else {
    r = dt_pipe_pump(&s->ends.in_src, NULL); /* own input unselected: drop */
    if (r < 0) {
      return r;
    }
  }
  for (size_t i = 0; i < s->n; ++i) {
    if (s->selected == i + 1) {
      r = dt_pipe_pump(s->sources[i], &s->ends.out_snk);
      if (r < 0) {
        return r;
      }
      moved += r;
    } else {
      r = dt_pipe_pump(s->sources[i], NULL); /* unselected: drain and drop */
      if (r < 0) {
        return r;
      }
    }
  }
  return moved;
}

static int selector_begin(dt_pipe *pipe) { return selector_run((selector *)pipe->data); }
static int selector_tick(dt_pipe *pipe) { return selector_run((selector *)pipe->data); }
static int selector_finalize(dt_pipe *pipe) { return selector_run((selector *)pipe->data); }

static dt_pipe_source *selector_get_source(dt_pipe *pipe) {
  return &((selector *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *selector_get_sink(dt_pipe *pipe) {
  return &((selector *)pipe->data)->ends.pub_sink;
}

dt_pipe *dt_pipe_selector_create(dt_pipe_source **sources, size_t len) {
  if (len > 0 && !sources) {
    return NULL;
  }
  for (size_t i = 0; i < len; ++i) {
    if (!sources[i]) {
      return NULL;
    }
  }
  selector *s = dt_malloc(sizeof(*s));
  if (!s) {
    return NULL;
  }
  if (len > 0) {
    s->sources = dt_malloc(len * sizeof(*s->sources));
    if (!s->sources) {
      dt_free(s);
      return NULL;
    }
    for (size_t i = 0; i < len; ++i) {
      s->sources[i] = sources[i];
    }
  } else {
    s->sources = NULL;
  }
  s->n = len;
  s->selected = 0;
  s->base.source = selector_get_source;
  s->base.sink = selector_get_sink;
  s->base.begin = selector_begin;
  s->base.tick = selector_tick;
  s->base.finalize = selector_finalize;
  s->base.data = s;
  dt_pipe_ends_init(&s->ends);
  return &s->base;
}

void dt_pipe_selector_select(dt_pipe *pipe, size_t idx) {
  ((selector *)pipe->data)->selected = idx;
}

void dt_pipe_selector_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  selector *s = (selector *)pipe; // base is the first member
  dt_pipe_ends_free(&s->ends);
  dt_free(s->sources);
  dt_free(s);
}

/* -- valve: gate the input to the output, or drop it ----------------------- */

typedef struct {
  dt_pipe base;
  dt_pipe_ends ends;
  int open; // 1 = pass input to output; 0 = drop input
} valve;

/* The operation: pump the buffered input to the output (open) or to NULL (closed,
 * draining and discarding it). */
static int valve_run(valve *v) {
  return dt_pipe_pump(&v->ends.in_src, v->open ? &v->ends.out_snk : NULL);
}

static int valve_begin(dt_pipe *pipe) { return valve_run((valve *)pipe->data); }
static int valve_tick(dt_pipe *pipe) { return valve_run((valve *)pipe->data); }
static int valve_finalize(dt_pipe *pipe) { return valve_run((valve *)pipe->data); }

static dt_pipe_source *valve_get_source(dt_pipe *pipe) {
  return &((valve *)pipe->data)->ends.pub_source;
}
static dt_pipe_sink *valve_get_sink(dt_pipe *pipe) {
  return &((valve *)pipe->data)->ends.pub_sink;
}

dt_pipe *dt_pipe_valve_create(void) {
  valve *v = dt_malloc(sizeof(*v));
  if (!v) {
    return NULL;
  }
  v->open = 1; // created open
  v->base.source = valve_get_source;
  v->base.sink = valve_get_sink;
  v->base.begin = valve_begin;
  v->base.tick = valve_tick;
  v->base.finalize = valve_finalize;
  v->base.data = v;
  dt_pipe_ends_init(&v->ends);
  return &v->base;
}

void dt_pipe_valve_open(dt_pipe *pipe) { ((valve *)pipe->data)->open = 1; }
void dt_pipe_valve_close(dt_pipe *pipe) { ((valve *)pipe->data)->open = 0; }

void dt_pipe_valve_destroy(dt_pipe *pipe) {
  if (!pipe) {
    return;
  }
  valve *v = (valve *)pipe; // base is the first member
  dt_pipe_ends_free(&v->ends);
  dt_free(v);
}
