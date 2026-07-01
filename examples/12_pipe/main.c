/*
 * 12 - Pipes: composing bit-stream plumbing with the pipe/ API.
 *
 * Every other example drives a single codec through its vtable. The pipe/ API is
 * different: it is a small toolkit for wiring bit streams together. Everything is
 * built from three interfaces (all over the dt_bit / dt_soft_bit alphabet):
 *
 *   dt_pipe_source - something you PULL a stream out of.
 *   dt_pipe_sink   - something you PUSH a stream into.
 *   dt_pipe        - a buffered converter with BOTH ends: push into its sink, tick
 *                    it, pull from its source. Input and output are buffered and
 *                    the work happens only in tick() - the two ends are not chained.
 *
 * A pipe is driven begin -> (push / tick / pull)* -> finalize. Concrete pipes wrap
 * codecs (encoder / decoder), convert domains (hardening / softening), route
 * streams (splitter / diverter / valve ...), or chain other pipes (pipeline).
 *
 * Part A: endpoints and dt_pipe_pump - copy a buffer source into a buffer sink.
 * Part B: one pipe's lifecycle - a softening pipe lifts hard bits to soft records.
 * Part C: a pipeline - chain an encoder pipe and a decoder pipe to round-trip a
 *         message (the headline: many stages, one dt_pipe you drive like any other).
 * Part D: fan-out - a splitter tees a stream to its own output AND a side sink.
 *
 * Run: ./12_pipe
 */

#include "util.h"

#include <drifty/cc/ccode.h>
#include <drifty/cc/encoder.h>
#include <drifty/cc/viterbi.h>
#include <drifty/pipe/buffers.h>
#include <drifty/pipe/multi.h>
#include <drifty/pipe/pipes.h>
#include <drifty/pipe/streams.h>

/* Drain a source's hard face into a printable 0/1 string (for display). */
static void show_bits(const char *label, const dt_bit *b, int n) {
  printf("  %-22s ", label);
  for (int i = 0; i < n; ++i) {
    putchar(b[i] == DT_TRUE ? '1' : '0');
  }
  printf("  (%d bits)\n", n);
}

int main(void) {
  uint64_t rng = 0x9174ECABu;

  /* ---- Part A: endpoints + dt_pipe_pump ---- */
  printf("Part A - a source, a sink, and dt_pipe_pump between them:\n");
  {
    enum { N = 24 };
    dt_bit in[N], out[N];
    ex_rand_bits(in, N, &rng);

    /* Buffer endpoints wrap a plain array as a source / sink (no allocation). */
    dt_pipe_buffer_source src;
    dt_pipe_buffer_sink dst;
    dt_pipe_source *s = dt_pipe_buffer_source_init(&src, in, N);
    dt_pipe_sink *k = dt_pipe_buffer_sink_init(&dst, out, N);

    int moved = dt_pipe_pump(s, k); /* copy everything the source has to the sink */
    show_bits("pumped in", in, N);
    show_bits("landed out", out, (int)dst.len);
    printf("  moved %d bits; output matches input: %s\n", moved,
           dst.len == N && ex_count_errors(in, out, 0, N) == 0 ? "yes" : "no");
  }

  /* ---- Part B: one pipe's lifecycle (a softening converter) ---- */
  printf("\nPart B - a single pipe: softening lifts hard bits to soft records.\n"
         "Push into its sink, tick to convert, pull records from its source:\n");
  {
    dt_pipe *soft = dt_pipe_softening_create();
    dt_pipe_sink *in = soft->sink(soft);
    dt_pipe_source *out = soft->source(soft);

    dt_bit bits[6];
    ex_rand_bits(bits, 6, &rng);
    soft->begin(soft);
    in->push(in, bits, 6);
    /* Nothing reaches the source until a tick moves it across - not chained. */
    dt_soft_bit peek[6];
    int before = out->soft_pull(out, peek, 6);
    soft->tick(soft); /* the conversion happens here */
    soft->finalize(soft);
    dt_soft_bit recs[8];
    int got = out->soft_pull(out, recs, 8);

    printf("  pulled before tick: %d records (buffered, not yet converted)\n", before);
    printf("  after tick: %d records - each hard bit is now one-hot soft:\n", got);
    for (int i = 0; i < got; ++i) {
      printf("    bit %s -> c_true=%.0f c_false=%.0f\n", ex_sym(bits[i]),
             (double)recs[i].c_true, (double)recs[i].c_false);
    }
    dt_pipe_softening_destroy(soft);
  }

  /* ---- Part C: a pipeline (encoder -> decoder round-trip) ---- */
  printf("\nPart C - a pipeline of codec pipes: [encoder | decoder] round-trips a\n"
         "message. The compound is itself a dt_pipe, driven like any other:\n");
  {
    enum { NINFO = 200 };
    dt_bit msg[NINFO];
    ex_rand_bits(msg, NINFO, &rng);

    dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
    dt_stream_encoder *enc = dt_cc_encoder_create(code);
    dt_stream_decoder *dec = dt_cc_viterbi_decoder_create(code);

    /* Wrap each codec as a pipe, then chain them. Pipes do not own the codecs. */
    dt_pipe *ep = dt_pipe_encoder_create(enc);
    dt_pipe *dp = dt_pipe_decoder_create(dec);
    dt_pipe *stages[2] = {ep, dp};
    dt_pipe *pl = dt_pipeline_create(stages, 2);

    dt_pipe_sink *in = pl->sink(pl);
    dt_pipe_source *out = pl->source(pl);

    dt_bit recovered[NINFO + 64];
    pl->begin(pl);
    in->push(in, msg, NINFO); /* push info bits into the front of the chain */
    pl->tick(pl);             /* encode -> (move) -> decode, all in one tick */
    pl->finalize(pl);         /* flush the encoder trailer and decoder tail */
    int n = 0, g;
    while ((g = out->pull(out, recovered + n, (int)(NINFO + 64 - n))) > 0) {
      n += g;
    }
    /* The decoder trails input by its decision depth; the recovered interior
     * matches the message exactly (0 errors) once aligned. */
    int errs = ex_count_errors(recovered, msg, 0, NINFO < n ? NINFO : n);
    printf("  %d info bits -> pipeline -> %d recovered bits, %d errors\n", NINFO, n,
           errs);

    dt_pipeline_destroy(pl);
    dt_pipe_encoder_destroy(ep);
    dt_pipe_decoder_destroy(dp);
    dt_cc_encoder_destroy(enc);
    dt_cc_viterbi_decoder_destroy(dec);
    dt_cc_code_destroy(code);
  }

  /* ---- Part D: fan-out with a splitter (a tee) ---- */
  printf("\nPart D - a splitter tees its input to its own output AND a side sink,\n"
         "so a monitor can watch a stream without disturbing it:\n");
  {
    enum { N = 20 };
    dt_bit in[N], mainout[N], monitor[N];
    ex_rand_bits(in, N, &rng);

    /* the side sink the splitter copies to (in addition to its own output) */
    dt_pipe_buffer_sink mon;
    dt_pipe_sink *mk = dt_pipe_buffer_sink_init(&mon, monitor, N);
    dt_pipe_sink *sinks[1] = {mk};
    dt_pipe *tee = dt_pipe_splitter_create(sinks, 1);

    dt_pipe_sink *tin = tee->sink(tee);
    dt_pipe_source *tout = tee->source(tee);
    tee->begin(tee);
    tin->push(tin, in, N);
    tee->tick(tee);
    tee->finalize(tee);
    int mo = 0, g;
    while ((g = tout->pull(tout, mainout + mo, (int)(N - mo))) > 0) {
      mo += g;
    }
    show_bits("input", in, N);
    show_bits("main output", mainout, mo);
    show_bits("monitor copy", monitor, (int)mon.len);
    printf("  output and monitor both match the input: %s\n",
           mo == N && mon.len == N && ex_count_errors(in, mainout, 0, N) == 0 &&
                   ex_count_errors(in, monitor, 0, N) == 0
               ? "yes"
               : "no");
    dt_pipe_splitter_destroy(tee);
  }

  printf("\nEvery piece here is a dt_pipe or an endpoint over the same two faces\n"
         "(hard dt_bit / soft dt_soft_bit), so they compose freely - see\n"
         "doc/pipe/README.md for the whole toolkit.\n");
  return 0;
}
