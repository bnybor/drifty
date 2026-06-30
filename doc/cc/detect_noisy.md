# drifty — `detect_noisy` meta-codec

A **blind detector** of convolutional-code structure in an arbitrary bit stream,
with no prior knowledge or coordination — no code, rate, generators, or alignment.
It is **soft-output only** and standalone: unlike the other [`cc/`](README.md)
decoders it is not built over a [`dt_cc_code`](ccode.md). It does not recover bit
values; it reports, per stream position, how confident it is that a convolutional
code is present.

`detect_noisy` is the **noise-tolerant** of drifty's two blind detectors. Where
[`detect_clean`](detect_clean.md) needs near-exact parity (it holds only to ~1 %
flips), detect_noisy scores the **bias** of approximate parity checks with a fast
Walsh–Hadamard transform, which degrades *gracefully* with noise: it tolerates flips
(to ~5 %, marginally to ~8 %), indels (to ~2–3 %), and **light–moderate combinations
of the two**. The cost is a **~64 KB transform histogram** and roughly one to two
orders more compute per bit. For a clean / very-low-noise channel where footprint
matters, prefer the cheaper [`detect_clean`](detect_clean.md); the two share this
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
is emitted per input bit (output trails input by up to one analysis window).

## Method — parity-check bias via a fast Walsh–Hadamard transform

A convolutional code is **linear**, so it has low-weight **parity checks**: a binary
vector `c` with `c · (window of coded bits) = 0` for every phase-aligned window.
Under a per-bit flip rate `p`, such a check still holds with probability
`(1 + (1 − 2p)^w)/2` (`w` = its Hamming weight), so its **bias**

```
  β(c) = | E[ (−1)^(c · row) ] |  ≈  (1 − 2p)^w  >  0,
```

while for random data every `c` gives `β ≈ 0`. The detector's statistic is the
**max bias over all candidate checks `c`**. Crucially a flip only *shrinks*
`(1 − 2p)^w` rather than destroying the check (contrast detect_clean's exact rank,
which a single flip breaks), so the bias degrades gracefully — with flips, with
indels (rows after a slip fall out of phase and merely stop contributing bias; the
aligned rows still bias), and with combinations of the two.

Computing the max bias over all `2^{L_c}` checks `c` at once is exactly a
**Walsh–Hadamard transform**: histogram the `L_c`-bit row-slices of a window,
transform in place, and the largest `|coefficient|` (over `c ≠ 0`) divided by the
row count is the max bias. The checks are **phase-specific** (they relate output
bits at a fixed position mod `n`, the block size), so the rows are taken at a
candidate **stride `s = n`** to stay phase-aligned; `n` is unknown, so detect sweeps
`s = 2..6`.

Windows **slide** (length `L`, step `L/4`) and each position is assigned the **max
evidence over the windows covering it**, so a code is found wherever a
sufficiently-aligned run exists and localization stays reasonable. The raw
per-window-per-stride evidence is the **excess** of the bias over the random floor,

```
  excess = β / f0 − 1,     f0(N) = sqrt(2 · L_c · ln2 / N)   (N = rows at stride s),
```

`f0` being the expected max bias of `N` random rows over `2^{L_c}` candidates
(`excess ≈ 0` for random, large for a clean code). The per-position max excess maps
to the verdict:

```
  c_erasure = clamp(excess / K, 0, 1)               (code-present, K a calibration constant)
  c_absent  = clamp(1 − excess, 0, 1) · detectability (no-code; see channel model)
```

Geometry: `L_c = 14` (the check span / transform order — `2^{14}` histogram, ~64 KB),
strides `2..6`, window `L = 1200` bits sliding by `300`, `K = 2`. Output trails
input by up to one window (~1200 bits).

### Measured envelope

Mean code-present (`c_erasure`) and no-code (`c_absent`) confidence on the K7-rate-½
preset, clean-channel model, over a long stream (`detect_noisy` self-tests and the
[`examples/11_detect`](../../examples/11_detect) demo exercise these points):

| Channel | `c_erasure` (code-present) | `c_absent` (no-code) |
|---------|:--------------------------:|:--------------------:|
| coded, clean            | **1.00** | 0.00 |
| coded, 1 % flip         | 1.00 | 0.00 |
| coded, 3 % flip         | 0.92 | 0.00 |
| coded, 5 % flip         | ~0.50 | ~0.13 |
| coded, 1.5 % deletion   | 0.69 | 0.00 |
| coded, 3 % flip + 0.5 % deletion | 0.33 | ~0.49 |
| **random**              | 0.02 | **0.96** |

Clean coded reads an unambiguous *present*, random an unambiguous *absent*, and the
evidence degrades smoothly in between rather than snapping off.

## API

```c
#include <drifty/cc/detect_noisy.h>

typedef struct {
  int   decision_depth;
  int   max_drift;
  float p_flip;
  float p_ins_true, p_ins_false, p_ins_erase;
  float p_del;
  float p_ovr_true, p_ovr_false, p_ovr_erase;
} dt_cc_detect_noisy_stream_params;

dt_stream_soft_decoder *dt_cc_detect_noisy_soft_decoder_create(const dt_cc_detect_noisy_stream_params *params);
void             dt_cc_detect_noisy_soft_decoder_destroy(dt_stream_soft_decoder *dec);
```

The factory takes the **same rich channel model as [`hybrid`](hybrid.md) /
[`maxir`](maxir.md)** (and [`detect_clean`](detect_clean.md)), so a channel you
already describe for an inner codec can be handed to detect unchanged (copied; need
not outlive the call). It returns NULL on a bad argument or out of memory. Drive the
soft decoder through its vtable — `begin → decode` (repeat) `→ finalize` — like any
[soft decoder](../stream.md); it uses no preamble.

### Channel-model parameters

detect_noisy's bias method **tolerates** flips, so flips are not what damages it; the
flip part of the channel model instead **calibrates how much a null result can be
trusted**. The more flip noise you tell detect_noisy to expect, the less a
random-looking stream can be confidently declared code-*free* — a true code's bias
decays as `(1 − 2·p_flip)^w`, so heavy expected flips could have pushed a real code's
bias down into the random floor. So the **`c_absent` (no-code) confidence is scaled
down** by a *detectability* factor `(1 − 2p)^{W_ref}` (`W_ref` a representative check
weight) where `p = p_ovr + (1−p_ovr)·p_flip`; at `p ≥ 0.5` no bias survives and
detectability is `0` (a code cannot be ruled out at all). The **`c_lost`
(code-present) confidence is never affected** — an observed bias is real regardless
of expected noise, since noise only erodes bias, never manufactures it.

| Field | Role in detect |
|-------|----------------|
| `p_flip` | expected coded-bit flip rate, `0 ≤ p_flip < 1`. `0` = "expect a clean channel" (unlike hybrid/maxir, which require `> 0`). Damps the no-code confidence. |
| `p_ovr_true` / `p_ovr_false` / `p_ovr_erase` | overwrite rates (sum `< 1`); count as flip-like corruption (damp the no-code confidence). |
| `p_ins_true` / `p_ins_false` / `p_ins_erase`, `p_del` | insertion / deletion rates (sum `< 1`) — the **drift detect is built to tolerate**. They do **not** damp the no-code confidence (the sliding windows recover from indels by finding aligned runs); accepted/validated as the expected drift. |
| `decision_depth` (`≥ 1`), `max_drift` (`≥ 0`) | accepted for interface uniformity with the cc family but **not used** by the bias method (detect has its own windowed delay; indel tolerance is intrinsic, not a drift window). Validated only. |

A clean channel (`p_flip = 0`, everything else 0) gives detectability `1`, so
`c_absent` is undamped — the default behaviour. Rough magnitudes are all that
matter.

## Limitations

- **Noise envelope — graceful but bounded.** Reliable to ~5 % flips (marginal to
  ~8 %) and ~2–3 % indels, plus *light–moderate* combinations. Heavy **simultaneous**
  flips + indels (e.g. 8 % + 2 %) stay out of reach — this is fundamental to the
  underlying learning-parity-with-noise + synchronization problem, not a tuning
  artefact. Where neither hypothesis dominates, the two confidences are both mid: the
  honest output is "uncertain", and detect_noisy keeps coded streams measurably above
  random even there.
- **Cost.** One `2^{L_c}`-entry histogram (~64 KB) plus a Walsh–Hadamard transform
  per window-stride — roughly one to two orders more work per bit than detect_clean,
  whose state is a few KB with no transform. On a clean channel detect_clean reaches
  the same verdict for far less, so spend detect_noisy only where the noise warrants.
- **Scope.** Parity-check bias senses *linear* redundancy in general — a block linear
  code or an LFSR scrambler would also register as "code present". For the intended
  use (a stream is either uncoded/random or convolutionally coded) this is the right
  proxy; detect reports "a code is present", not "specifically a convolutional code".
- **Reach.** Checks longer than `L_c = 14` bits, or codes with block size `n > 6`,
  are not covered — this caps the detectable constraint length / lowest rate.

## See also

- [`detect_clean`](detect_clean.md) — the cheap, embeddable sibling (same API and
  output) for clean / very-low-noise streams.
- [Soft decoding](../soft_decoding.md) and [Symbols (`bit`)](../bit.md) — the
  `dt_soft_bit` fields detect repurposes.
- [Convolutional coding (`cc/`)](README.md) — the codec family detect senses.
