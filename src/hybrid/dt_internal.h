#ifndef DRIFTY_DT_INTERNAL_H
#define DRIFTY_DT_INTERNAL_H

#include <stdint.h>

/*
 * Definition of the opaque dt_code, shared by the encoder (encode.c) and the
 * decoder (decode.c). Private to the library build - not installed.
 */
struct dt_code {
  int K;                    /* constraint length                   */
  int n;                    /* output bits per input bit           */
  int n_states;             /* 1 << (K-1)                          */
  unsigned int input_tap;   /* 1 << (K-1)                          */
  unsigned int *generators; /* [n]                                 */
  int *next_state;          /* [n_states*2], indexed [s*2 + b]     */
  uint8_t *output;          /* [n_states*2*n], [((s*2)+b)*n + j]   */
};

#endif /* DRIFTY_DT_INTERNAL_H */
