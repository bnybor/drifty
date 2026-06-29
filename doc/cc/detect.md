# drifty — `detect` meta-codec

A **blind detector** of convolutional-code structure in an arbitrary bit stream,
with no prior knowledge or coordination — no code, rate, generators, or alignment.
It is **soft-output only** and standalone: unlike the other [`cc/`](README.md)
decoders it is not built over a [`dt_cc_code`](ccode.md) and takes **no
parameters**. It does not recover bit values; it reports, per stream position, how
confident it is that a convolutional code is present.

## Output

detect populates two fields of the output [`dt_soft_bit`](../bit.md) (each a
consistency in `[0, 1]`; they need **not** sum to 1), all others 0:

| Field | Meaning |
|-------|---------|
| `c_erasure` | confidence a convolutional code **is** encoded onto the stream here |
| `c_absent`  | confidence a convolutional code is **not** encoded onto the stream here |

The coded-presence confidence rides in `c_erasure` by the engine convention
(internal `c_lost` → soft `c_erasure`); detect repurposes that field. One record
is emitted per input bit (output trails input by up to one analysis block).

## Method — GF(2) strided-window rank deficiency

A convolutional code is **linear and time-invariant**, so its output bits satisfy
parity-check relations: a width-`W` window of coded bits lives in a proper GF(2)
subspace, so a matrix built from such windows is **rank-deficient**, while random
bits span the full space (full rank). The deficiency `d = W − rank` counts the
parity checks visible in the window.

The subtlety: a code's parity checks are **phase-specific** (they relate output
bits at a fixed position mod `n`, the block size). So the window rows must be
stacked at a **stride equal to `n`** to stay phase-aligned — stacking at stride 1
mixes phases and the matrix is full-rank even for coded data. Since `n` is unknown,
detect **sweeps candidate strides** `s = 2..6` and takes the largest deficiency
`d = max_s (W − rank_s)`; a code of block size `n` shows up at `s = n` (and its
multiples). Per analysis block of `BLOCK` input bits it computes one verdict:

- `d = 0` → no linear structure → **no code** (`c_absent` high).
- `d > 0` → parity checks found → **code present** (`c_erasure = 1 − 2^{−d}`).

Geometry: `W = 32`, strides `2..6` (block sizes `n ∈ {2..6}` — the rate-1/n presets
are `n ∈ {2,3,5}`), `BLOCK = 384` (so the widest stride still gives ≥ 32 rows for a
well-determined null space). On the standard presets the clean-stream deficiency is
`d = 10 / 15 / 21` for K7-rate-½ / K7-rate-⅓ / K5-rate-⅕ → `c_erasure ≈ 1`, while a
random stream gives `d = 0` → `c_absent ≈ 1`.

## API

```c
#include <drifty/cc/detect.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_flip;
  float p_ins_true, p_ins_false, p_ins_erase;
  float p_del;
  float p_ovr_true, p_ovr_false, p_ovr_erase;
} dt_cc_detect_stream_params;

dt_stream_soft_decoder *dt_cc_detect_soft_decoder_create(const dt_cc_detect_stream_params *params);
void             dt_cc_detect_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

The factory takes the **same rich channel model as [`hybrid`](hybrid.md) /
[`maxir`](maxir.md)**, so a channel you already describe for an inner codec can be
handed to detect unchanged (copied; need not outlive the call). It returns NULL on
a bad argument or out of memory. Drive the soft decoder through its vtable —
`begin → decode` (repeat) `→ finalize` — like any [soft decoder](../stream.md); it
uses no preamble.

### Channel-model parameters

detect's rank method needs **exact** parity to see a code, which any corruption
breaks — so the channel model is used not to decode but to **calibrate how much a
null result can be trusted**. The more corruption you tell detect to expect, the
less a clean-looking stream can be confidently declared code-*free* (a code could
be present but hidden by the noise), so the **`c_absent` (no-code) confidence is
scaled down** by a *detectability* factor `(1 − p)^W` where `p` is the expected
per-bit corruption. The **`c_lost` (code-present) confidence is never affected** —
parity checks that are actually found are real regardless of expected noise, since
noise only destroys structure, never creates it.

| Field | Role in detect |
|-------|----------------|
| `p_flip` | expected coded-bit flip rate, `0 ≤ p_flip < 1`. `0` = "expect a clean channel" (unlike hybrid/maxir, which require `> 0`). |
| `p_ovr_true` / `p_ovr_false` / `p_ovr_erase` | overwrite rates (sum `< 1`); all count as corruption. |
| `p_ins_true` / `p_ins_false` / `p_ins_erase`, `p_del` | insertion / deletion rates (sum `< 1`). Drift breaks the strided-window phase, so it reduces detectability sharply. |
| `decision_depth` (`≥ 1`), `max_drift` (`≥ 0`) | accepted for interface uniformity with the cc family but **not used** by the rank method (detect has its own block-based delay and does not track drift). Validated only. |

A clean channel (`p_flip = 0`, everything else 0) gives detectability `1`, so
`c_absent` is undamped — the default behaviour. Rough magnitudes are all that
matter.

## Limitations

- **Noise.** A single bit flip breaks exact parity over the windows overlapping it,
  so exact GF(2) rank is brittle: a noisy coded stream reads full-rank and so
  detects as *absent*. detect targets the **clean / very-low-noise** regime — it
  holds to ~0.5 % bit flips (`d` degrades but stays positive) and collapses to
  "absent" by ~1 %. A noise-tolerant detector needs approximate low-weight parity
  checks / syndrome statistics / LPN-style methods (future work).
- **Scope.** Rank deficiency senses *linear* redundancy in general — a block linear
  code or an LFSR scrambler would also register as "code present". For the intended
  use (a stream is either uncoded/random or convolutionally coded) this is the
  right proxy; detect reports "a code is present", not "specifically a
  convolutional code".
- **Block size.** The stride sweep covers block sizes `n ≤ 6`; codes with larger
  `n` would need a wider sweep.

## See also

- [Soft decoding](../soft_decoding.md) and [Symbols (`bit`)](../bit.md) — the
  `dt_soft_bit` fields detect repurposes.
- [Convolutional coding (`cc/`)](README.md) — the codec family detect senses.
