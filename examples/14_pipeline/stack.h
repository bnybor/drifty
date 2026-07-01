/*
 * 14 - stack.h: the OUTER concatenated stack (rs251 block code + marker framing) that
 * rides above the funnel's inner convolutional layer. Transmit builds framed rs251
 * blocks through the frame ENCODER pipe; receive walks the funnel's final frame SOFT
 * decoder pipe and rs251-decodes each frame. Not part of drifty - example scaffolding.
 */

#ifndef EX14_STACK_H
#define EX14_STACK_H

#include <drifty/bit.h>
#include <drifty/pipe/pipe.h>

/* Outer RS(n, k) over GF(251); each frame carries one block. */
#define RS_N 40
#define RS_K 24
#define RS_MSG (RS_K - 1)  /* payload bytes per frame  */
#define CW_BITS (RS_N * 8) /* codeword bits per frame  */
#define FRAMES 6           /* frames (RS blocks) per signal region */

/* payload rows -> rs251 blocks -> marker frames, built through the frame encoder pipe.
 * Writes the framed bit stream to out[] (cap bits) and returns its length. */
int stack_build_framed(unsigned char payload[][RS_MSG], int nframes, dt_bit *out,
                       int cap);

/* Walk the final frame SOFT-decoder pipe boundary by boundary (tick / advance); each
 * in-frame run is one frame, rs251 soft-decoded and matched against the `npay` sent
 * payloads. Reports frames delimited, payloads recovered, and residual inner-decoder
 * bit errors the outer RS mopped up. Drives and finalizes `frame_pipe`. */
void stack_recover(dt_pipe *frame_pipe, unsigned char payloads[][RS_MSG], int npay,
                   int *found, int *recovered, int *residual);

#endif /* EX14_STACK_H */
