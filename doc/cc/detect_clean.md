# drifty — `detect_clean` meta-codec

A **blind detector** of convolutional-code structure in an arbitrary bit stream,
with no prior knowledge or coordination — no code, rate, generators, or alignment.
It is **soft-output only** and standalone: unlike the other [`cc/`](README.md)
decoders it is not built over a [`dt_cc_code`](ccode.md). It does not recover bit
values; it reports, per stream position, how confident it is that a convolutional
code is present.

`detect_clean` is the **cheap, embeddable** one of drifty's two blind detectors: exact
GF(2) rank deficiency, a few KB of state, no transform. It tolerates indels and
**~1 % flips** — the clean / very-low-noise regime. For a channel with real flip
noise (and combinations of flips and drift), reach for the heavier
[`detect_noisy`](detect_noisy.md), which scores parity-check *bias* and shares this
codec's API and output.

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

## Method — GF(2) sliding strided-window rank deficiency

A convolutional code is **linear and time-invariant**, so its output bits satisfy
parity-check relations: a width-`W` window of coded bits lives in a proper GF(2)
subspace, so a matrix built from such windows is **rank-deficient**, while random
bits span the full space (full rank). The deficiency `d = W − rank` counts the
parity checks visible in the window.

The checks are **phase-specific** (they relate output bits at a fixed position mod
`n`, the block size), so the window rows must be stacked at a **stride equal to
`n`** to stay phase-aligned — stacking at stride 1 mixes phases and the matrix is
full-rank even for coded data. Since `n` is unknown, detect **sweeps candidate
strides** `s = 2..6`; a code of block size `n` shows up at `s = n` (and multiples).

The windows **slide**, which is what makes detection indel-tolerant and sharply
localized. Any row that spans an insertion/deletion — or strays out of a coded
region into random bits — is independent of the code's subspace, so it fills the
rank and erases the deficiency: a matrix is deficient **only when all its rows lie
inside one indel-free, phase-aligned coded run**. detect slides a short window
(length `L_s = s·(W+MARGIN)+W`, chosen so every stride yields the same row count
`N = W+MARGIN+1` and the same random rejection `2^{−(N−W)}`) and assigns each
position the **max deficiency over the windows covering it**:

- `d = 0` → no clean aligned run here → **no code** (`c_absent` high).
- `d > 0` → parity checks found → **code present** (`c_erasure = 1 − 2^{−d}`).

So a code is detected wherever a *locally* clean aligned run exists — an indel only
kills the windows that span it, not the runs between them — and a window crossing a
code/random boundary reads `d = 0`, keeping localization sharp.

Geometry: `W = 32`, `MARGIN = 18`, strides `2..6` (block sizes `n ∈ {2..6}` — the
rate-1/n presets are `n ∈ {2,3,5}`), windows of `132…332` bits sliding by `32`. On
the standard presets the clean-run deficiency is `d = 10 / 15 / 21` for K7-rate-½ /
K7-rate-⅓ / K5-rate-⅕ → `c_erasure ≈ 1`; a random stream gives `d = 0` →
`c_absent ≈ 1`. Output trails input by up to one longest window (~332 bits).

## API

```c
#include <drifty/cc/detect_clean.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_flip;
  float p_ins_true, p_ins_false, p_ins_erase;
  float p_del;
  float p_ovr_true, p_ovr_false, p_ovr_erase;
} dt_cc_detect_clean_stream_params;

dt_stream_soft_decoder *dt_cc_detect_clean_soft_decoder_create(const dt_cc_detect_clean_stream_params *params);
void             dt_cc_detect_clean_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

The factory takes the **same rich channel model as [`hybrid`](hybrid.md) /
[`maxir`](maxir.md)** (and [`detect_noisy`](detect_noisy.md)), so a channel you
already describe for an inner codec can be handed to detect unchanged (copied; need
not outlive the call). It returns NULL on a bad argument or out of memory. Drive the
soft decoder through its vtable — `begin → decode` (repeat) `→ finalize` — like any
[soft decoder](../stream.md); it uses no preamble.

### Channel-model parameters

detect's rank method needs **exact** parity within a window to see a code, which a
flip breaks — so the flip part of the channel model is used not to decode but to
**calibrate how much a null result can be trusted**. The more flip noise you tell
detect to expect, the less a clean-looking stream can be confidently declared
code-*free* (a code could be present but hidden by the flips), so the **`c_absent`
(no-code) confidence is scaled down** by a *detectability* factor `(1 − p)^W` where
`p = p_ovr + (1−p_ovr)·p_flip` is the expected per-bit flip/overwrite corruption.
The **`c_lost` (code-present) confidence is never affected** — parity checks that
are actually found are real regardless of expected noise, since noise only destroys
structure, never creates it.

| Field | Role in detect |
|-------|----------------|
| `p_flip` | expected coded-bit flip rate, `0 ≤ p_flip < 1`. `0` = "expect a clean channel" (unlike hybrid/maxir, which require `> 0`). Damps the no-code confidence. |
| `p_ovr_true` / `p_ovr_false` / `p_ovr_erase` | overwrite rates (sum `< 1`); count as flip-like corruption (damp the no-code confidence). |
| `p_ins_true` / `p_ins_false` / `p_ins_erase`, `p_del` | insertion / deletion rates (sum `< 1`) — the **drift detect is built to tolerate**. They do **not** damp the no-code confidence (the sliding windows recover from indels by finding clean runs); accepted/validated as the expected drift. |
| `decision_depth` (`≥ 1`), `max_drift` (`≥ 0`) | accepted for interface uniformity with the cc family but **not used** by the rank method (detect has its own windowed delay; indel tolerance is intrinsic, not a drift window). Validated only. |

A clean channel (`p_flip = 0`, everything else 0) gives detectability `1`, so
`c_absent` is undamped — the default behaviour. Rough magnitudes are all that
matter.

## Limitations

- **Flips.** A flipped bit is an independent row in every window that covers it, so
  it breaks that window's deficiency. detect_clean targets the **clean /
  very-low-noise** regime for flips: it holds to ~1 % bit flips (`d` degrades but
  stays positive) and collapses to "absent" beyond that. For a channel that actually
  flips bits, use [`detect_noisy`](detect_noisy.md) — it scores approximate parity
  *bias* (a Walsh–Hadamard transform), which flips only weaken rather than destroy,
  holding to ~5–8 % flips at a ~64 KB / heavier-compute cost.
- **Indels — tolerated.** Insertions and deletions shift the bit phase (which
  desyncs an ordinary decoder), but the sliding windows only need a *locally*
  indel-free aligned run, so detection survives sparse drift, degrading gracefully:
  it holds well to ~1 % and remains usable to ~2–3 % indels.
- **Scope.** Rank deficiency senses *linear* redundancy in general — a block linear
  code or an LFSR scrambler would also register as "code present". For the intended
  use (a stream is either uncoded/random or convolutionally coded) this is the
  right proxy; detect reports "a code is present", not "specifically a
  convolutional code".
- **Block size.** The stride sweep covers block sizes `n ≤ 6`; codes with larger
  `n` would need a wider sweep.

## See also

- [`detect_noisy`](detect_noisy.md) — the flip-tolerant sibling (same API and output).
- [Soft decoding](../soft_decoding.md) and [Symbols (`bit`)](../bit.md) — the
  `dt_soft_bit` fields detect repurposes.
- [Convolutional coding (`cc/`)](README.md) — the codec family detect senses.
