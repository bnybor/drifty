# drifty — the convolutional code (`dt_cc_code`)

A `dt_cc_code` is the convolutional **code** itself — the redundancy scheme the
[encoder](encoder.md) and every decoder ([`viterbi`](viterbi.md),
[`bcjr`](bcjr.md), [`vindel`](vindel.md), [`hybrid`](hybrid.md),
[`maxir`](maxir.md)) are built over. It is the one object they all share: the
sender and receiver must use the *same* code, and a decoder built for one code
will not lock onto another's stream. Everything else in [`cc/`](README.md) is
parameterised by it.

The header is [`ccode.h`](../../include/drifty/cc/ccode.h). For the symbol
alphabet the coded stream speaks, see
[Data-flow semantics](../data_flow_semantics.md).

## Lifecycle

```c
#include <drifty/cc/ccode.h>

dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
/* ... build an encoder and a decoder over `code`, use them ... */
dt_cc_code_destroy(code);
```

`dt_cc_code` is an opaque handle. Create one (either factory below), build an
encoder and decoders over it, and free it with `dt_cc_code_destroy` (NULL is
fine). **The code must outlive everything built from it** — the encoder and every
decoder hold a borrowed pointer to it, not a copy.

A code carries no per-stream state, so one `dt_cc_code` may back any number of
encoders and decoders at once.

## Ready-made codes

`dt_cc_code_create_standard(which)` returns one of a fixed catalogue, selected by
the `dt_cc_standard_code` enum. Returns NULL on a bad argument or out of memory.

| Constant | Rate | Coded bits / input bit | Free distance `d_free` |
|----------|------|:----------------------:|:----------------------:|
| `DT_CC_CODE_K3_RATE_1_2` | 1/2 | 2× | 5 |
| `DT_CC_CODE_K7_RATE_1_2` | 1/2 | 2× | 10 |
| `DT_CC_CODE_K7_RATE_1_3` | 1/3 | 3× | 15 |
| `DT_CC_CODE_K5_RATE_1_5` | 1/5 | 5× | 20 |

Higher `d_free` corrects more, at the cost of more coded bits per input bit (more
bandwidth). When unsure, `DT_CC_CODE_K7_RATE_1_2` is the general default;
`K5_RATE_1_5` is the strongest against drift (the [metrics](../../metrics)
sweeps make this case). The trailing number in the name is the constraint length
`K`; the rate fixes the bandwidth expansion.

### Alternates

Each rate/`K` family also has a few **alternates** named `…_ALT1`, `…_ALT2`, …
(`DT_CC_CODE_K7_RATE_1_2_ALT1`, etc.). They exist so you can run **several
independent streams that will not be mistaken for one another**: every code in
the catalogue is chosen to be mutually distinguishable under the decoders'
lock metric, so a decoder for one never false-locks onto another's stream. How
many a family has depends on how many distinguishable codes its generator space
holds — the rate-1/2 families carry three apiece, the rate-1/3 and rate-1/5
families five. An alternate trades a little free distance for that separation;
the default (un-suffixed) member is the strongest of its family.

> The catalogue's generator sets and their `d_free` values are listed in full,
> with per-alternate free distances, in the enum comments in
> [`ccode.h`](../../include/drifty/cc/ccode.h).

## Custom codes

Most users want a standard code; if you need a specific generator polynomial,
build one directly:

```c
/* The classic constraint-length-7, rate-1/2 code (octal generators 171, 133) —
 * exactly what DT_CC_CODE_K7_RATE_1_2 builds. */
static const unsigned int gens[] = {0171, 0133};
dt_cc_code *code = dt_cc_code_create(/*K=*/7, gens, /*num_generators=*/2);
```

- **`K`** is the **constraint length** — the width of the code's shift register,
  in the range `2..9`. The register holds the current input bit plus the previous
  `K − 1`. (`decision_depth ≈ 6·K` is a good decoder setting; see the decoder
  pages.)
- **`num_generators`** is how many coded bits each input bit becomes — the
  denominator of the rate. It is what [`dt_cc_code_n()`](#accessors) reports.
- **`generators`** is an array of `num_generators` **tap masks**, one per coded
  output. Each is a `K`-bit number: bit `K − 1` (value `1 << (K−1)`, the
  most-significant) taps the **current** input bit, and each lower bit taps a
  progressively older one, down to bit 0 for the oldest. Coded output *j* is the
  **parity (XOR)** of the shift-register bits its generator selects. Masks are
  conventionally written in **octal**, matching the coding-theory literature (the
  `0171, 0133` above is the standard Odenwalder pair).

`dt_cc_code_create` returns NULL on a bad argument (`generators` NULL, `K`
outside `2..9`, or `num_generators < 1`) or out of memory. The resulting code is
used exactly like a standard one.

A code is **systematic only if** one of its generators is `1 << (K−1)` (the
input tap alone, octal `0100` at `K = 7`); the standard catalogue codes are
non-systematic, so the information bits are not visible verbatim in the coded
stream.

## Accessors

```c
int dt_cc_code_n(const dt_cc_code *code);  /* coded bits per input bit */
int dt_cc_code_k(const dt_cc_code *code);  /* constraint length K      */
```

- **`dt_cc_code_n`** — coded output bits per input bit (the rate denominator).
  The coded length of `m` input bits is `m · dt_cc_code_n(code)`; size encoder
  output buffers from it (the [encoder page](encoder.md#sizing-the-output) gives
  the exact bound, including the flush tail). Returns `−1` if `code` is NULL.
- **`dt_cc_code_k`** — the constraint length `K`. A good `decision_depth` is
  about `6·K`. Returns `−1` if `code` is NULL.

## See also

- [encoder](encoder.md) — the shared encoder built over a code.
- The decoders: [`viterbi`](viterbi.md), [`bcjr`](bcjr.md),
  [`vindel`](vindel.md), [`hybrid`](hybrid.md), [`maxir`](maxir.md).
- [Worked example: concatenation](../concatenated.md) — a code driven end to end
  through the inner encoder, a channel, and an outer block code.
