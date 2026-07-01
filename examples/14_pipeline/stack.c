/*
 * 14 - stack.c: the outer rs251 + marker frame stack (see stack.h).
 */

#include "stack.h"

#include "util.h"

#include <drifty/bc/rs251.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/fc/marker.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/pipe/frames.h>
#include <drifty/result.h>

/* byte <-> bit packing is ex_byte_to_bits / ex_bits_to_byte in util.h. */

int stack_build_framed(unsigned char payload[][RS_MSG], int nframes, dt_bit *out,
                       int cap) {
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_frame_encoder *fe = dt_fc_marker_frame_encoder_create();
  dt_pipe *ep = dt_pipe_frame_encoder_create(fe);
  ep->begin(ep);
  dt_pipe_sink *in = ep->sink(ep);
  for (int f = 0; f < nframes; ++f) {
    dt_bit *rin = rse->decoded_buf(rse);
    for (int i = 0; i < RS_MSG; ++i) {
      ex_byte_to_bits(payload[f][i], rin + i * 8);
    }
    rse->reset(rse);
    dt_result r;
    while ((r = rse->encode(rse)) == DT_AGAIN) {
    }
    const dt_bit *cw = rse->encoded_buf(rse);
    dt_pipe_frame_encoder_begin_frame(ep); /* open a frame */
    in->push(in, cw, CW_BITS);
    ep->tick(ep); /* encode the codeword inside the frame */
    dt_pipe_frame_encoder_end_frame(ep);   /* close it */
  }
  ep->finalize(ep);
  dt_pipe_source *src = ep->source(ep);
  int len = 0, g;
  while ((g = src->pull(src, out + len, (size_t)(cap - len))) > 0) {
    len += g;
  }
  dt_pipe_frame_encoder_destroy(ep);
  dt_fc_marker_frame_encoder_destroy(fe);
  dt_bc_rs251_block_encoder_destroy(rse);
  return len;
}

void stack_recover(dt_pipe *fp, unsigned char payloads[][RS_MSG], int npay, int *found,
                   int *recovered, int *residual) {
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_block_soft_decoder *rsd = dt_bc_rs251_block_soft_decoder_create(RS_N, RS_K, 0);
  dt_pipe_source *src = fp->source(fp);
  static dt_soft_bit run[CW_BITS * 4];
  const int rcap = (int)(sizeof run / sizeof *run);
  *found = *recovered = *residual = 0;
  int finalized = 0, guard = 0;
  for (;;) {
    dt_frame_decoder_state before = dt_pipe_frame_soft_decoder_get_state(fp);
    int w = fp->tick(fp); /* copy one run up to the next frame boundary */
    int got = 0, g;
    while ((g = src->soft_pull(src, run + got, (size_t)(rcap - got))) > 0) {
      got += g;
    }
    dt_frame_decoder_state after = dt_pipe_frame_soft_decoder_get_state(fp);
    if (before == DT_FRAME_DECODER_INSIDE && got == CW_BITS) { /* one whole frame */
      ++*found;
      memcpy(rsd->encoded_buf(rsd), run, (size_t)CW_BITS * sizeof(dt_soft_bit));
      rsd->reset(rsd);
      dt_result r;
      while ((r = rsd->decode(rsd)) == DT_AGAIN) {
      }
      if (r == DT_OK) {
        const dt_bit *rb = rsd->decoded_buf(rsd);
        unsigned char m[RS_MSG];
        int ok = 1;
        for (int b = 0; b < RS_MSG && ok; ++b) {
          ok = ex_bits_to_byte(rb + b * 8, &m[b]);
        }
        int which = -1;
        for (int p = 0; p < npay && ok; ++p) {
          if (memcmp(m, payloads[p], RS_MSG) == 0) {
            which = p;
            break;
          }
        }
        if (which >= 0) {
          ++*recovered;
          dt_bit *rin = rse->decoded_buf(rse);
          for (int b = 0; b < RS_MSG; ++b) {
            ex_byte_to_bits(payloads[which][b], rin + b * 8);
          }
          rse->reset(rse);
          dt_result er;
          while ((er = rse->encode(rse)) == DT_AGAIN) {
          }
          const dt_bit *cw = rse->encoded_buf(rse);
          for (int b = 0; b < CW_BITS; ++b) {
            if (ex_hard_of(run[b]) != cw[b]) {
              ++*residual;
            }
          }
        }
      }
    }
    if (w == 0 && before == after) {
      if (finalized) {
        break;
      }
      fp->finalize(fp); /* flush the last in-flight frame, then drain it next pass */
      finalized = 1;
      continue;
    }
    dt_pipe_frame_soft_decoder_advance(fp); /* step past this boundary */
    if (++guard > 1000000) {
      break;
    }
  }
  dt_bc_rs251_block_soft_decoder_destroy(rsd);
  dt_bc_rs251_block_encoder_destroy(rse);
}
