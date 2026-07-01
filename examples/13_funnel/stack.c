/*
 * 13 - stack.c: the outer rs251 + marker frame stack (see stack.h).
 */

#include "stack.h"

#include "util.h"

#include <drifty/bc/rs251.h>
#include <drifty/block_encoder.h>
#include <drifty/block_soft_decoder.h>
#include <drifty/fc/marker.h>
#include <drifty/frame_encoder.h>
#include <drifty/frame_soft_decoder.h>
#include <drifty/result.h>

int stack_build_framed(unsigned char payload[][RS_MSG], int nframes, dt_bit *out,
                       int cap) {
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_frame_encoder *fe = dt_fc_marker_frame_encoder_create();
  int len = fe->begin(fe, out, cap);
  for (int f = 0; f < nframes; ++f) {
    dt_bit *rin = rse->decoded_buf(rse); /* (RS_K-1)*8 info bits */
    for (int i = 0; i < RS_MSG; ++i) {
      ex_byte_to_bits(payload[f][i], rin + i * 8);
    }
    rse->reset(rse);
    dt_result r;
    while ((r = rse->encode(rse)) == DT_AGAIN) {
    }
    const dt_bit *cw = rse->encoded_buf(rse); /* RS_N*8 codeword bits */
    len += fe->begin_frame(fe, out + len, cap - len);
    len += fe->encode(fe, out + len, cap - len, cw, CW_BITS);
    len += fe->end_frame(fe, out + len, cap - len);
  }
  len += fe->finalize(fe, out + len, cap - len);
  dt_fc_marker_frame_encoder_destroy(fe);
  dt_bc_rs251_block_encoder_destroy(rse);
  return len;
}

struct stack_result stack_recover(const dt_soft_bit *soft, int slen,
                                  unsigned char payload[][RS_MSG], int nframes) {
  /* Drive the marker SOFT frame decoder one bit at a time so it can watch the frame
   * boundaries: a maximal INSIDE run is one frame. Walking it means a corrupt delimiter
   * desyncs only its own frame - the decoder resynchronises on the next marker. */
  int cap = slen + 1024;
  dt_soft_bit *blob = malloc((size_t)cap * sizeof *blob);   /* all in-frame bits */
  int *bound = malloc((size_t)(slen / 8 + 16) * sizeof *bound); /* frame-end offsets */
  int nin = 0, nb = 0;

  dt_frame_soft_decoder *fd = dt_fc_marker_frame_soft_decoder_create();
  fd->begin(fd, NULL, 0);
  dt_frame_decoder_state prev = fd->get_state(fd);
  dt_soft_bit ob[64];
  for (int p = 0; p <= slen; ++p) {
    int nn = (p < slen) ? fd->decode(fd, ob, 64, &soft[p], 1)
                        : fd->finalize(fd, ob, 64);
    dt_frame_decoder_state s = fd->get_state(fd);
    if (prev == DT_FRAME_DECODER_INSIDE) {
      for (int e = 0; e < nn && nin < cap; ++e) {
        blob[nin++] = ob[e];
      }
    }
    if (prev == DT_FRAME_DECODER_INSIDE && s != DT_FRAME_DECODER_INSIDE) {
      bound[nb++] = nin; /* frame just closed */
    }
    prev = s;
  }
  if (prev == DT_FRAME_DECODER_INSIDE && (nb == 0 || bound[nb - 1] != nin)) {
    bound[nb++] = nin; /* a frame still open at end of stream */
  }
  dt_fc_marker_frame_soft_decoder_destroy(fd);

  /* rs251 soft-decode each full-length frame; re-encode a match to count the residual
   * inner-decoder errors it corrected. */
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(RS_N, RS_K);
  dt_block_soft_decoder *rsd = dt_bc_rs251_block_soft_decoder_create(RS_N, RS_K, 0);
  struct stack_result res = {nb, 0, 0, 0};
  int lo = 0;
  for (int fr = 0; fr < nb; ++fr) {
    const dt_soft_bit *f = blob + lo;
    int flen = bound[fr] - lo;
    lo = bound[fr];
    if (flen != CW_BITS) { /* a corrupt delimiter left this frame the wrong size */
      continue;
    }
    ++res.full;
    memcpy(rsd->encoded_buf(rsd), f, (size_t)CW_BITS * sizeof(dt_soft_bit));
    rsd->reset(rsd);
    dt_result r;
    while ((r = rsd->decode(rsd)) == DT_AGAIN) {
    }
    if (r != DT_OK) {
      continue;
    }
    const dt_bit *rb = rsd->decoded_buf(rsd);
    unsigned char got[RS_MSG];
    int ok = 1;
    for (int b = 0; b < RS_MSG && ok; ++b) {
      ok = ex_bits_to_byte(rb + b * 8, &got[b]);
    }
    int which = -1;
    for (int fp = 0; fp < nframes && ok; ++fp) {
      if (memcmp(got, payload[fp], RS_MSG) == 0) {
        which = fp;
        break;
      }
    }
    if (which < 0) {
      continue;
    }
    ++res.recovered;
    dt_bit *rin = rse->decoded_buf(rse);
    for (int b = 0; b < RS_MSG; ++b) {
      ex_byte_to_bits(payload[which][b], rin + b * 8);
    }
    rse->reset(rse);
    dt_result er;
    while ((er = rse->encode(rse)) == DT_AGAIN) {
    }
    const dt_bit *cw = rse->encoded_buf(rse);
    for (int b = 0; b < CW_BITS; ++b) {
      if (ex_hard_of(f[b]) != cw[b]) {
        ++res.residual;
      }
    }
  }
  dt_bc_rs251_block_soft_decoder_destroy(rsd);
  dt_bc_rs251_block_encoder_destroy(rse);
  free(blob);
  free(bound);
  return res;
}
