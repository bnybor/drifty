/*
 * 13 - stack.h: the OUTER concatenated stack (rs251 block code + marker framing) that
 * rides above the funnel's inner convolutional layer. Transmit builds framed rs251
 * blocks; receive reframes a decoded soft stream and rs251-decodes each frame. Not
 * part of drifty - example scaffolding.
 */

#ifndef EX13_STACK_H
#define EX13_STACK_H

#include <drifty/bit.h>
#include <drifty/soft_bit.h>

/* Outer RS(n, k) over GF(251); each frame carries one block. */
#define RS_N 40
#define RS_K 24
#define RS_MSG (RS_K - 1)  /* payload bytes per frame  */
#define CW_BITS (RS_N * 8) /* codeword bits per frame  */
#define FRAMES 6           /* frames (RS blocks) per signal region */

/* What one region's outer recovery produced. */
struct stack_result {
  int found;     /* frames delimited by the marker decoder */
  int full;      /* of those, exactly CW_BITS long (a usable codeword) */
  int recovered; /* payloads rs251 recovered and matched to a sent frame */
  int residual;  /* inner-decoder bit errors rs251 mopped up over the recovered frames */
};

/* payload rows -> rs251 blocks -> marker frames. Writes the framed bit stream to
 * out[] (cap bits) and returns its length. */
int stack_build_framed(unsigned char payload[][RS_MSG], int nframes, dt_bit *out,
                       int cap);

/* Reframe a decoded soft stream (the marker SOFT frame decoder, walked so a corrupt
 * delimiter desyncs only its own frame) and rs251 soft-decode each frame, matching
 * recovered payloads against the `nframes` sent rows. */
struct stack_result stack_recover(const dt_soft_bit *soft, int slen,
                                  unsigned char payload[][RS_MSG], int nframes);

#endif /* EX13_STACK_H */
