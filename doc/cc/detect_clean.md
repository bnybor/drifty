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
| `c_erasure` | consistency with "a convolutional code **is** present here" |
| `c_absent`  | consistency with "**no** code / the stream is random here" |

These are two INDEPENDENT goodness-of-fit reads, not a probability split: each answers
"does the data fail to contradict this hypothesis?", so they need **not** sum to 1.
`(high, low)` reads as a code, `(low, high)` as random; **`(1, 1)`** means *no
discriminating evidence* — an all-non-bit run (e.g. all-erasure) or the warm-up /
flush tail, where nothing observed contradicts either hypothesis — and `(low, low)`
is unreachable from the single rank statistic (reserved for a future
catalogue-mismatch signal).

A **`DT_INVALID`** symbol is **two-sided evidence**. Encoders emit invalids only in
*runs*, so a placement no single encoder could emit — a lone invalid, or runs of
**differing length** — both contradicts a code (damps `c_erasure` toward 0) **and**, as
the hallmark of a non-coded source, favors no-code (raises `c_absent` toward 1):
scattered invalids read **`(low, high)`**. An invalid in an *encodable* shape (a single
contiguous run, or several of equal length) — and a whole window of invalids, itself one
run — moves neither axis, reading `(1, 1)`. Other non-bits
(`DT_ERASURE`/`DT_ABSENT`/`DT_NONE`) are neutral don't-knows and damp neither axis.

The code-present read rides in `c_erasure` by the engine convention (internal
`c_lost` → soft `c_erasure`). One record is emitted per input bit (output trails input
by up to one analysis block).

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

- `d ≥ 1` → parity checks found → **consistent with a code** (`c_erasure = 1`); random
  would need a `2^{−d}` fluke to fake this, so `c_absent = 2^{−d}` (low). True whatever
  the channel — noise erodes structure, it never manufactures it.
- `d = 0` → no structure. Its absence never contradicts random, so `c_absent = 1`; a
  code is ruled out (`c_erasure → 0`) only insofar as the fill is confirmed (margin
  rows beyond `W`) **and** the channel is clean enough that a real code's parity could
  not have been flipped into full rank: `c_erasure = 1 − detectability · fillconf`
  (see the channel model).
- never covered (the tail, or an all-non-bit run) → **`(1, 1)`**, no evidence.

Overlaid on the `d` verdict is a **`DT_INVALID` placement** check, independent of `d`: a
row carrying a non-bit is dropped from the rank as a don't-know, but an invalid whose
*placement* no single encoder could emit is **two-sided** evidence. Each un-encodable
**unit** — a singleton (a length-1 invalid run), plus each extra distinct run-length
present — damps `c_erasure` by `4^{−1}` (contradicts a code) **and** lifts `c_absent`
toward 1 by the same factor (favors no-code: scattered un-encodable symbols are the
signature of a non-coded source). A single invalid run, several of equal length, or a
whole window of invalids is an encodable shape and adds no penalty. So a coded window
spliced with lone invalids reads `(low, high)`; a *random* window with the same invalids
stays a confident `(low, high)` no-code — the lift also cancels the spurious deficiency
that thinning the rank would otherwise leave, so scattered invalids never read as more
code-like — and an all-erasure run carrying them reads `(low, 1)`.

So a code is detected wherever a *locally* clean aligned run exists — an indel only
kills the windows that span it, not the runs between them — and a window crossing a
code/random boundary reads `d = 0`, keeping localization sharp.

Geometry: `W = 32`, `MARGIN = 18`, strides `2..6` (block sizes `n ∈ {2..6}` — the
rate-1/n presets are `n ∈ {2,3,5}`), windows of `132…332` bits sliding by `32`. On
the standard presets the clean-run deficiency is `d = 10 / 15 / 21` for K7-rate-½ /
K7-rate-⅓ / K5-rate-⅕ → `c_erasure ≈ 1`; a random stream gives `d = 0` →
`c_absent ≈ 1` (and, under a clean model, `c_erasure ≈ 0`). Output trails input by up
to one longest window (~332 bits).

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
**calibrate how strongly an unstructured (full-rank) window is allowed to rule a code
OUT**. The more flip noise you tell detect to expect, the more a clean-looking
full-rank window could still be a real code whose parity the flips destroyed, so the
**code-present read `c_erasure` is held up** rather than collapsing to 0 — by a factor
`1 − detectability`, where the *detectability* `(1 − p)^W` (`p = p_ovr + (1−p_ovr)·p_flip`,
the expected per-bit flip/overwrite corruption) is roughly how likely a real code's
parity would have *survived* the expected noise. The **`c_absent` (no-code) read is
never scaled** — an unstructured window is consistent with random whatever the
channel, and a *found* deficiency contradicts random regardless of noise (noise only
destroys structure, never creates it). So the model moves only the **code-present
axis**, and only on full-rank windows.

| Field | Role in detect |
|-------|----------------|
| `p_flip` | expected coded-bit flip rate, `0 ≤ p_flip < 1`. `0` = "expect a clean channel" (unlike hybrid/maxir, which require `> 0`). Holds the code-present read up on full-rank windows. |
| `p_ovr_true` / `p_ovr_false` / `p_ovr_erase` | overwrite rates (sum `< 1`); count as flip-like corruption (hold the code-present read up). |
| `p_ins_true` / `p_ins_false` / `p_ins_erase`, `p_del` | insertion / deletion rates (sum `< 1`) — the **drift detect is built to tolerate**. They affect **neither** read (the sliding windows recover from indels by finding clean runs); accepted/validated as the expected drift. |
| `decision_depth` (`≥ 1`), `max_drift` (`≥ 0`) | accepted for interface uniformity with the cc family but **not used** by the rank method (detect has its own windowed delay; indel tolerance is intrinsic, not a drift window). Validated only. |

A clean channel (`p_flip = 0`, everything else 0) gives detectability `1`, so a
full-rank window rules a code out fully (`c_erasure → 0`) — the default behaviour.
Rough magnitudes are all that matter.

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
  it holds well to ~1 % and remains usable to ~2 % indels.
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
