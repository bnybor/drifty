# drifty `cc` — Clean-Room Reimplementation Specification

**Subject:** the convolutional-coding (`cc`) subsystem of the `drifty` library
(github.com/bnybor/drifty), comprising the shared code object, the encoder, five
decoders (`viterbi`, `bcjr`, `vindel`, `hybrid`, `maxir`), and two blind
code-presence detectors (`detect_clean`, `detect_noisy`).

**Purpose:** this document describes the externally observable behavior and the
internal algorithms in enough detail that an independent implementer, working
only from this text, can produce a functionally equivalent implementation —
bit-identical hard decisions where the algorithms are deterministic, and
numerically equivalent soft outputs up to floating-point associativity.

Where a detail is *load-bearing* for equivalence (tie-break order, loop order,
ring sizing, exact constants) it is called out as **NORMATIVE**. Where a detail
is an implementation convenience (buffer growth policy, scratch layout) it is
marked *advisory*.

---

## 0. Global conventions

- All arithmetic on path metrics is single-precision `float` in
  **negative-log-likelihood (NLL) cost units**: lower is better, `+INFINITY`
  means infeasible. `-log` means natural log. The reference implementation is
  freestanding (`-ffreestanding -fno-builtin -nostdlib`) and uses its own
  single-precision `exp`/`log` proxies (`dt_exp`, `dt_log`); any faithful
  single-precision `expf`/`logf` is acceptable for equivalence purposes.
  `-log(0)` must evaluate to `+INFINITY` (the code relies on this: e.g.
  `cost_erase = -log(p_erase)` with `p_erase == 0` yields an infeasible branch
  that is provably never taken).
- Streams are unbounded; every decoder is a **streaming** object with an
  internal received-symbol buffer, a `feed → produce` cadence, and a `flush`
  (drain) mode.
- One symbol per byte, never packed. The element type is `uint8_t`
  (`dt_bit`).
- Counts and indices are `int`; monotone step counters (`steps`, `decided`,
  `committed`, absolute stream offsets) are `long long`.
- Error codes: `DT_OK = 0`, `DT_ERR_ARG = -1`, `DT_ERR_ALLOC = -2`,
  `DT_AGAIN = -3`, `DT_ERR_DECODE = -4`. Decode entry points return a
  non-negative count of outputs written, or a negative error code.

---

## 1. The symbol alphabet

### 1.1 Hard symbols (`dt_bit`)

The whole API speaks a six-symbol alphabet. The native unit is the **position**
(a slot in the bit grid); a **value** is a property a position may or may not
carry. Symbols are points in a five-flag space so that every predicate is a
single mask test.

**NORMATIVE flag bits:**

| flag | value | meaning |
|---|---|---|
| `DT_DELETED` | `0x10` | decoder asserts the slot is missing (output only) |
| `DT_PRESENT` | `0x08` | the slot exists in the transmit grid |
| `DT_BOUND`   | `0x04` | a value is specified for the slot |
| `DT_BOOLEAN` | `0x02` | the bound value is a truth value |
| `DT_VALUE`   | `0x01` | the truth value itself (meaningful iff `DT_BOOLEAN`) |

**NORMATIVE symbol encodings:**

| symbol | encoding | semantics |
|---|---|---|
| `DT_NONE`    | `0x00` | not a symbol; uninitialized storage. In no data flow. |
| `DT_ABSENT`  | `0x10` (`DELETED`) | decoder-inferred deletion / cannot place. Output-domain only. |
| `DT_ERASURE` | `0x08` (`PRESENT`) | present position, no value bound. Inbound: don't-care deferred to the channel. Outbound: don't-know. |
| `DT_INVALID` | `0x0C` (`PRESENT|BOUND`) | bound to a deliberate non-boolean value ("poison"). Round-trips: a sent `DT_INVALID` recovers as `DT_INVALID`, damage permitting. |
| `DT_FALSE`   | `0x0E` (`PRESENT|BOUND|BOOLEAN`) | bound false |
| `DT_TRUE`    | `0x0F` (`PRESENT|BOUND|BOOLEAN|VALUE`) | bound true |

**Predicates** (each a single mask test): `DT_IS_TRANSMIT(s) = s & PRESENT`
(T/F/E/I — valid encoder/decoder input); `DT_IS_OUTPUT(s) = s & (PRESENT|DELETED)`;
`DT_IS_BOUND(s) = s & BOUND` (T/F/I); `DT_IS_BIT(s) = s & BOOLEAN` (T/F);
`DT_BIT(s) = s & VALUE` (0/1 payload, meaningful only when `DT_IS_BIT`);
`DT_IS_ABSENT(s) = s & DELETED`.

A decoder-internal helper used everywhere the poison must be recognized:

```
is_invalid_sym(s) := (s & DT_BOUND) && !(s & DT_BOOLEAN)
```

This deliberately matches any bound non-boolean, not just the exact `0x0C`
encoding.

### 1.2 Soft output (`dt_soft_bit`)

Per recovered information position, six `float` fields, each a **consistency in
[0, 1]** — a goodness-of-fit of one hypothesis against the received stream, NOT
a probability split (fields need not sum to 1):

`c_false`, `c_true`, `c_erasure` (value unrecoverable), `c_invalid` (coded
support arrived as encoder poison), `c_absent` (position deleted / not
synchronized), `c_locked` (decoder is tracking a valid stream of this code —
independent of the value fields).

Decoder engines internally name the erasure field `c_lost` and the lock field
`c_lock`; the public wrapper maps `c_lost → c_erasure`, `c_lock → c_locked`
one-to-one.

---

## 2. The code object (`dt_cc_code`)

A rate-1/n binary convolutional code with constraint length K, shared by
encoder and every decoder. Opaque handle; created from `(K, generators[n], n)`.

### 2.1 Parameters and validation

- `K` ∈ [2, 9] (**NORMATIVE** upper bound: keeps `1 << (K-1)` clear of shift UB
  and bounds every table). `num_generators = n ≥ 1`.
- `n_states = 1 << (K-1)`; `input_tap = 1 << (K-1)`.

### 2.2 Register and trellis convention (**NORMATIVE**)

The K-bit shift register at a step is:

```
shift_register = (input_bit << (K-1)) | state
```

i.e. **bit K−1 is the newest input**, and the (K−1)-bit `state` holds the K−1
previous inputs with **bit K−2 the most recent** and bit 0 the oldest. Each
generator is a K-bit tap mask in the same orientation (bit K−1 taps the current
input). Output bit j of edge `(state, bit)`:

```
output[j] = parity(shift_register & generators[j])     // XOR of tapped bits
```

State advance:

```
next_state = ((state >> 1) | (bit << (K-2))) & (n_states - 1)
```

Precompute two flat tables indexed by `edge = state*2 + bit`:
`next_state[n_states*2]` and `output[n_states*2*n]` (row-major, `[edge*n + j]`,
values raw 0/1).

Under this convention the K=7 pair `{0171, 0133}` (octal) is the standard
CCSDS/Voyager code (g₀ = 1+D+D²+D³+D⁶, g₁ = 1+D²+D³+D⁵+D⁶ with D = delay),
which fixes the bit-order unambiguously.

### 2.3 Standard code catalogue (**NORMATIVE**)

Each family is a default plus alternates chosen (by an offline search) to be
mutually distinguishable under the decoders' lock metric — a decoder for one
will not lock onto another's stream. Octal generator sets:

| enum | K | generators (octal) | d_free |
|---|---|---|---|
| `K3_RATE_1_2` | 3 | 005, 007 | 5 |
| `K3_RATE_1_2_ALT1` | 3 | 001, 007 | 4 |
| `K3_RATE_1_2_ALT2` | 3 | 003, 007 | 4 |
| `K7_RATE_1_2` | 7 | 0171, 0133 | 10 |
| `K7_RATE_1_2_ALT1` | 7 | 0043, 0175 | 8 |
| `K7_RATE_1_2_ALT2` | 7 | 0107, 0156 | 8 |
| `K7_RATE_1_3` | 7 | 0113, 0135, 0157 | 15 |
| `K7_RATE_1_3_ALT1` | 7 | 0112, 0153, 0157 | 14 |
| `K7_RATE_1_3_ALT2` | 7 | 0037, 0135, 0153 | 13 |
| `K7_RATE_1_3_ALT3` | 7 | 0012, 0145, 0177 | 12 |
| `K7_RATE_1_3_ALT4` | 7 | 0042, 0133, 0172 | 12 |
| `K5_RATE_1_5` | 5 | 025, 027, 033, 035, 037 | 20 |
| `K5_RATE_1_5_ALT1` | 5 | 007, 017, 025, 027, 035 | 18 |
| `K5_RATE_1_5_ALT2` | 5 | 011, 032, 033, 035, 037 | 18 |
| `K5_RATE_1_5_ALT3` | 5 | 013, 021, 023, 033, 037 | 17 |
| `K5_RATE_1_5_ALT4` | 5 | 013, 024, 032, 033, 037 | 17 |

Accessors: `dt_cc_code_n(code)` → n, `dt_cc_code_k(code)` → K (each −1 on
NULL). Recommended `decision_depth ≈ 6·K`.

---

## 3. The encoder

One encoder serves every codec. It is the ordinary convolutional encoder
extended to carry non-boolean inputs through to **marked coded bits**, using
two shadow registers with the same shift geometry as the state register.

### 3.1 State carried between calls

- `int state` — the (K−1)-bit encoder state, caller-initialized to 0.
- `unsigned int unknown` — two packed (K−1)-bit shadow registers,
  caller-initialized to 0. **NORMATIVE packing:** the *erasure register* in
  bits [0,7] and the *invalid register* in bits [8,15]
  (`MASK_BITS = 8`; K ≤ 9 keeps each register ≤ 8 bits).

### 3.2 Per-input-bit emission (`emit_group`) (**NORMATIVE**)

For each input symbol `b`:

```
in_bit     = DT_IS_BIT(b)                    // T or F
in_invalid = !in_bit && DT_IS_BOUND(b)       // DT_INVALID (poison)
in_erasure = !in_bit && !DT_IS_BOUND(b)      // DT_ERASURE (unbound)
bit        = DT_BIT(b)                       // 0 for any non-boolean
```

Build full K-bit shadow registers by placing the new flag at `input_tap`:

```
era_reg = (in_erasure ? input_tap : 0) | era_carried
inv_reg = (in_invalid ? input_tap : 0) | inv_carried
```

Then for each of the n output taps `j`:

1. if `generators[j] & inv_reg` → emit `DT_INVALID` (poison dominates — it
   cannot be concretized by the channel);
2. else if `generators[j] & era_reg` → emit `DT_ERASURE` (the parity is
   unbound; deferred to the channel, which may concretize it);
3. else emit `DT_TRUE`/`DT_FALSE` per the precomputed `output` table for
   `(state, bit)`.

Per-output-bit precedence is therefore **INVALID > ERASURE > clean**. The
definite `bit = 0` for a non-boolean input only advances the trellis; by
linearity, exactly the taps that would carry the unknown value are marked, so
the value never leaks into a clean output and the deferral is unbiased.

Advance all three registers in lockstep (`top_bit = 1 << (K-2)`,
`state_mask = n_states - 1`):

```
state    = next_state[state*2 + bit]
era_next = ((era_carried >> 1) | (in_erasure ? top_bit : 0)) & state_mask
inv_next = ((inv_carried >> 1) | (in_invalid ? top_bit : 0)) & state_mask
unknown  = (inv_next << 8) | era_next
```

### 3.3 API

- `encode(code, bits, n_bits, &state, &unknown, out)` — encodes `n_bits`
  symbols into `n_bits * n` coded symbols; returns bits written or
  `DT_ERR_ARG` (NULL args, negative counts, `state` out of `[0, n_states)`).
  Chunked encoding of one continuous stream is supported by passing the same
  `state`/`unknown` to every call.
- `flush(code, &state, &unknown, out)` — feeds K−1 literal zero bits
  (`in_erasure = in_invalid = 0`), producing `(K-1)*n` trailing coded symbols.
  This returns the state register to 0 and shifts any in-flight unknown flags
  out, so the decoder can recover the final input bits at full confidence.

### 3.4 Encoder property the decoders rely on (**NORMATIVE**)

A `DT_INVALID` input poisons *exactly* the coded bits that would carry its
value. Consequently, at the originating step the decoder has **no clean value
evidence at all** — the forced-value path costs tie exactly (`m0 == m1`) — and
the poison round-trips as `DT_INVALID` through the tie-splitting cascade
(§6.5/§9.6). A clean input bit that is merely *contaminated downstream* by a
neighbouring poison keeps clean parity elsewhere and resolves to its true
value. Implementations must preserve this exactness (poison bits must be
value-free in the branch metric, see §4.4 and §6.2).

---

## 4. Shared machinery of the drift-tolerant decoders

`vindel`, `hybrid`, and `maxir` share one architecture: a **(state × drift)
super-trellis** advanced by a min-sum forward pass whose branch metrics come
from a per-step **bit-level alignment DP**, with **re-anchoring** to keep
drift bounded, an **EWMA lock metric** with **re-acquisition**, and **blind
acquisition** initialization. This section defines those pieces once; the
per-decoder sections state only their differences.

### 4.1 The super-trellis

- `max_drift ≥ 0` is a parameter; `drift_width = 2*max_drift + 1`.
- A node is `(state, drift_index)`, `drift_index ∈ [0, drift_width)`, flat
  index `state * drift_width + drift_index` (**NORMATIVE layout** — state-major).
  `drift_index == max_drift` means zero local drift.
- Node metric arrays `metric[]` / `next_metric[]` of
  `count = n_states * drift_width` floats.
- One trellis **step** consumes one input bit's group of n coded bits at a
  drift-adjusted read position. A branch from source drift `di` to ending
  drift `nd` consumes `n + (nd − di)` received bits; the feasible range is
  `nd ≥ max(0, di − n)` (consumed ≥ 0) and `nd < drift_width` (the high bound
  `consumed ≤ n + 2*max_drift` is automatically satisfied).

### 4.2 Received buffer and read cursor

- Received symbols are buffered verbatim (`hybrid`/`maxir` keep raw `dt_bit`
  so `DT_INVALID` stays distinct; `vindel` normalizes on ingest, §7.1).
- `read_base` is the buffer index of the current step's **zero-drift** read
  base. The group base for source drift `di` is
  `base(di) = read_base + (di − max_drift)`.
- After each step, `read_base += n`.
- Buffer compaction may drop everything below `read_base − 2*max_drift`
  (history a backward re-anchor could still touch); `bcjr` instead keeps back
  to the oldest uncommitted step's group (it re-reads the buffer in its
  backward sweep). *Advisory:* compaction/growth policy is free as long as
  these retention floors hold.

### 4.3 Re-anchoring (**NORMATIVE**)

Before each step, compute the frontier (flat index of the minimum-metric node;
ties resolved to the **lowest flat index** by strict `<` scanning in ascending
index order) and its drift `drift = (frontier % drift_width) − max_drift`.
With `deadband = (max_drift + 1) / 2` (integer division):

```
sigma = +1 if drift >= deadband, −1 if drift <= −deadband, else 0
(sigma = 0 always when max_drift == 0)
```

If `sigma ≠ 0`: shift the window — each destination `drift_index` takes the
metric from source `drift_index + sigma`, out-of-window sources become
`+INFINITY` — and advance the read cursor: `read_base += sigma`. Record
`shift[step] = sigma` in a ring (the traceback / backward pass must translate
node coordinates across re-anchors). Emptied edge slots being `+INFINITY`
guarantees they are never recorded as predecessors, which keeps every
cross-re-anchor coordinate translation in range.

The net cumulative drift is thereby unbounded while each node's stored drift
stays inside ±max_drift.

### 4.4 Per-step match-cost tables (**NORMATIVE**)

Once per step, precompute over the window of `n + 4*max_drift` received
positions starting at `match_lo = read_base − max_drift`:

- `in_range[p]` — 1 iff `match_lo + p ∈ [0, received_length)`.
- `match_cost0[p]` / `match_cost1[p]` — cost of aligning an expected 0 / 1 at
  that position;
- `ins_cost[p]` — cost of consuming that position as an insertion
  (`vindel` uses a single `cost_ins`).

Per received symbol r at an in-range position (hybrid/maxir):

| r | match_cost0 | match_cost1 | ins_cost |
|---|---|---|---|
| `DT_ERASURE` (exact) | `keep + cost_erase` | `keep + cost_erase` | `cost_ins_e` |
| invalid (`is_invalid_sym`) | `keep` | `keep` | `cost_ins_e` |
| boolean, value v | `keep + cost_bit[0][v]` | `keep + cost_bit[1][v]` | `v ? cost_ins_t : cost_ins_f` |

The poison row is value-FREE (`cost_keep` only, identical on both expected
values) so a poisoned group stays a value-symmetric tie; it is deliberately
NOT folded into the erasure branch (which is `+inf` when `p_ovr_erase == 0`
and would make poisoned groups unalignable).

### 4.5 The per-edge alignment DP (`align_fill_into`) (**NORMATIVE**)

Given an edge's expected n-bit output row and a group base `base`, fill a DP
table `T[j][c]` (`j ∈ [0,n]` expected bits emitted, `c ∈ [0,max_consume]`
received bits consumed, `max_consume = n + 2*max_drift`, row stride
`max_consume + 1`) = minimum cost to align the first j expected bits while
consuming c received bits:

```
off = base − match_lo
T[0][0] = 0
T[0][c] = in_range[off+c−1] ? T[0][c−1] + ins_cost[off+c−1] : +INF   (c ≥ 1)
for j = 1..n, for c = 0..max_consume:
  best = T[j−1][c] + cost_del                       // delete expected[j−1]; always available
  if c ≥ 1 and in_range[off+c−1]:
    p = off + c − 1
    best = min(best, T[j−1][c−1] + match_cost_{expected[j−1]}[p])   // match/substitute
    best = min(best, T[j][c−1]   + ins_cost[p])                      // insert (consume extra)
  T[j][c] = best
```

Only rows 0..n−1 need scratch; row n (the **final row**) is the branch-cost
vector by total consumption. The branch cost from source drift `di` into
ending drift `nd` is `T[n][n + (nd − di)]`.

Out-of-buffer positions permit **only deletion**, which is what makes the
stream ends safe. Every matched/substituted or inserted bit pays its cost from
the tables of §4.4, so `cost_keep` (the per-bit "not an indel" cost) is
charged on matches, making the model fully bit-level rather than per-group.

### 4.6 Output-pattern dedup (**NORMATIVE for cost, advisory for method**)

Many of the `2*n_states` edges share the same n-bit output row (at most `2^n`
distinct rows). An edge's alignment depends only on (output row, source
drift), so:

- Register the code's **distinct output patterns** in first-encounter edge
  order (edge index ascending), assign `group_of[edge] = pattern index`.
- Per step, compute the final rows once per (pattern, live source drift) into
  a shared table `align_shared[(pattern*drift_width + di)*stride]`,
  `stride = max_consume + 1`. A drift is computed only if some state's metric
  is finite there (dead drifts are never read).
- The forward pass reads `align_shared` via `group_of[edge]`.

### 4.7 The forward pass (min-plus scatter) (**NORMATIVE order**)

With `next_metric` initialized to `+INF`, iterate **state ascending → source
drift ascending → bit 0 then 1 → ending drift ascending**, skipping infinite
sources; relax:

```
first = max(0, di − n)
for nd in [first, drift_width):
  cost = metric[state,di] + final_row[n + (nd − di)]
  next_metric[next_state[edge], nd] = min(prev, cost)      // strict < to replace
```

The strict `<` (keep-first on ties) in this exact iteration order is what
makes `vindel`'s backpointers — and hence its decoded bits — deterministic;
`hybrid`/`maxir` have no backpointers but keep the identical order so all
three produce identical path metrics. An infeasible branch is `+INF` and never
wins the min, so no explicit guard is needed inside the innermost loop.

### 4.8 Normalization and the lock signal (**NORMATIVE**)

After each scatter, find the frontier minimum; if it is finite and > 0,
subtract it from every finite node (adding a constant to a whole layer changes
no decision). The subtracted amount `increment` is the best path's per-step
cost — the lock signal. Update a trailing EWMA:

```
alpha = 2 / (decision_depth + 1)
smoothed_cost += alpha * (increment − smoothed_cost)
smoothed_cost is initialized to expected_unlock ("assume unlocked until proven")
```

Map to a lock consistency:

```
lock = clamp((expected_unlock − smoothed_cost) / (expected_unlock − expected_lock), 0, 1)
       (0 if the gap ≤ 0)
```

`expected_lock` / `expected_unlock` are channel-derived per-step reference
costs (per-decoder formulas in §6.3/§7.2/§8.2). This makes lock
**code-specific**: a confidently decoded *wrong* code is dominant but
expensive and reads as unlocked; two encoders of the same code share codewords
and read as locked.

### 4.9 Re-acquisition (**NORMATIVE**)

Constant `LOCK_MIN = 0.5`. After each step:

- if `lock ≥ 0.5`: set `locked_once = 1`, `unlock_run = 0`;
- else if `locked_once` and `++unlock_run ≥ reacquire_after`
  (`reacquire_after = 2 * decision_depth`): re-seed the trellis with the blind
  init (§4.10), set `smoothed_cost = expected_unlock`, `unlock_run = 0`,
  `locked_once = 0`.

The decoder thereby re-acquires sync downstream of a sustained loss instead of
staying stuck; the unlocked stretch surfaces as `DT_ABSENT` in the soft
decoders' cascade (hard-only `vindel` has no marker; the stretch reads as
ordinary bits, with the per-bit lock output flagging it).

### 4.10 Blind acquisition init (**NORMATIVE**)

All nodes `+INF` except every encoder state at **zero drift**
(`drift_index = max_drift`) at cost 0 — every state equally likely, so the
decoder locks whether it starts at the head of a stream or taps in mid-stream.
(`viterbi` is the exception: it seeds only state 0, §5.)

---

## 5. `viterbi` — synchronous hard-decision decoder

The cheapest decoder: a plain sliding-window Viterbi over the encoder-state
trellis. **No drift** (received index == transmitted index; each group of n
received symbols is exactly one step), **no channel parameters** (built from
the code alone), **known start state**.

### 5.1 Configuration

- `decision_depth = 6 * K` (fixed, derived from the code).
- Initial metric: `metric[0] = 0`, all other states `VIT_INF` (an integer
  sentinel, `INT_MAX`; infinite predecessors are skipped before any add so the
  sentinel never enters arithmetic). Because the encoder starts at state 0,
  output is reliable from the first bit.

### 5.2 Branch metric (**NORMATIVE**)

Integer Hamming distance over value-carrying symbols only:

```
cost(edge, group) = Σ_j [ DT_IS_BIT(group[j]) && DT_BIT(group[j]) != expected[j] ]
```

Any non-boolean received symbol (`DT_ERASURE`, `DT_INVALID`, anything else)
contributes 0 to **every** edge — the zero-reliability limit of a soft bit; it
drops out of the compare-select.

### 5.3 Step, traceback, cadence

- Add-compare-select in **state ascending, bit 0 then 1** order with strict
  `<` replacement, recording a packed backpointer
  (`bit:1 | prev_state:15`, 16-bit) per destination into a ring of
  `decision_depth` layers (`layer = steps % decision_depth`).
- After the scatter, renormalize by subtracting the frontier minimum from all
  reachable states (preserves every argmin).
- **Cadence:** before processing step `steps` (which would overwrite the ring
  layer of step `steps − decision_depth`), if `steps ≥ decision_depth`, emit
  the decision for step `decided` by walking backpointers from the frontier
  state (minimum metric, lowest state on ties) back to `decided`; the edge
  *into* the target step carries the emitted bit; `decided++`. Output symbols
  are always `DT_TRUE`/`DT_FALSE`.
- **Flush:** first run the normal cadence on whole buffered groups; then emit
  the residual `decided < steps` tail by tracing from the final frontier at
  reduced depth.

---

## 6. `bcjr` — synchronous max-log-MAP soft decoder

Forward–backward (max-log approximation of BCJR) over the plain encoder
trellis. Same synchronous channel as `viterbi` plus a parametric flip/erasure
model; produces the full soft alphabet. **Independent implementation from
`maxir`** (the drift-tolerant sibling): they share structure, not code.

### 6.1 Parameters and validation

`decision_depth ≥ 1`; `0 < p_flip < 1`; `0 ≤ p_erase < 1`.

### 6.2 Channel costs (**NORMATIVE**)

```
cost_match = −log((1 − p_erase)(1 − p_flip))
cost_miss  = −log((1 − p_erase) · p_flip)
cost_erase = −log(p_erase)            // +INF when p_erase == 0 (never read)
```

Per-symbol cost against an expected bit e:

- boolean r: `DT_BIT(r) == e ? cost_match : cost_miss`;
- invalid (`is_invalid_sym`): **0** — value-free AND cost-free, so a poisoned
  run keeps lock without favouring a value;
- anything else (erasure, stray symbol): `cost_erase` — neutral across edges
  but **penalized**, so a sustained erasure burst degrades lock.

The explicit `(1 − p_erase)` factor keeps paths that read different erasure
counts comparable; with `p_erase = 0` the metric reduces to plain
hard-decision NLL.

Branch cost `gamma(edge, base)` = sum of the n per-symbol costs of the group
at buffer index `base`, or `+INF` if the group is not fully buffered (guards
the stream ends). **There is a single definition of gamma, used by both
passes** — the backward sweep recomputes it from the retained buffer rather
than snapshotting it (memory/recompute trade, opposite of `maxir`).

### 6.3 Lock anchors (**NORMATIVE**)

```
misfit_lock   = p_flip
misfit_unlock = 0.5 · (p_flip + 0.5)
erase_term    = p_erase > 0 ? p_erase · cost_erase : 0
kept          = 1 − p_erase
expected_lock   = n · (erase_term + kept·((1−misfit_lock)·cost_match + misfit_lock·cost_miss))
expected_unlock = n · (erase_term + kept·((1−misfit_unlock)·cost_match + misfit_unlock·cost_miss))
```

### 6.4 Windowed batched forward–backward (**NORMATIVE**)

- `batch = decision_depth`; `ring_len = decision_depth + batch + 2`
  (= `2·decision_depth + 2`); `reacquire_after = 2·decision_depth`.
- The forward pass is a Viterbi butterfly **without backpointers** (decisions
  come from the α/β combine). Before the pass of step t, snapshot the entering
  state metric into `alpha_ring[t % ring_len]`; after the pass and EWMA
  update, snapshot `lock_ring[t % ring_len] = smoothed_cost`. Then run the
  re-acquisition rule (§4.9).
- The buffer origin bookkeeping keeps
  `received_origin + read_base == steps · n`, so step t's group base is
  `t·n − received_origin`; compaction retains the buffer back to the oldest
  uncommitted step's group.
- **Sweep trigger:** producing output drains an internal FIFO first; when the
  FIFO is empty and the forward pass can step (next group fully buffered), it
  steps; once `steps − committed ≥ decision_depth + batch`, one backward sweep
  emits `emit_hi = committed + batch − 1` (i.e. `batch` decisions) into the
  FIFO and sets `committed = emit_hi + 1`. When draining (flush), a final
  sweep with `emit_hi = steps − 1` empties the window (the tail gets reduced
  look-ahead).
- **Backward sweep:** seed `beta_tilde[state] = 0` for all states (the
  frontier is open — a path may end anywhere). For `t` from `steps − 1` down
  to `committed`: recompute gamma into `branch[edge]` for step t; then for
  each `(state, bit)` with `g = branch[edge] ≠ INF` and
  `bt = beta_tilde[next_state] ≠ INF`, `inner = g + bt`;
  `beta_cur[state] = min(beta_cur[state], inner)`; when `t ≤ emit_hi` and
  `alpha_t[state] ≠ INF`, fold `alpha_t[state] + inner` into `m0`/`m1` per the
  edge's bit. Normalize `beta_cur` (bounds it; never changes a decision), emit
  if in range, swap `beta_tilde ↔ beta_cur`.

### 6.5 Projection and hard cascade (**NORMATIVE**)

For step t with combined `(m0, m1)`, `mmin = min(m0, m1)`;
`c_lock = lock_from(lock_ring[t % ring_len])`; `c_invalid` = fraction of step
t's n received symbols that are `is_invalid_sym` (read from the retained
buffer).

- If `mmin == INF` (no complete path): `c_true = c_false = c_lost = 0`,
  `c_absent = 1 − c_lock`, symbol `DT_ABSENT`.
- Else `c_true = exp(mmin − m1)`, `c_false = exp(mmin − m0)` (winner reads
  exactly 1, loser `exp(−gap)`); `c_lost = min(c_true, c_false)`
  (≡ `1 − |c_true − c_false|`); `c_absent = 1 − c_lock`.
- Hard symbol — value-recoverability-first cascade with thresholds
  `LOCK_MIN = 0.5`, `INVALID_MIN = 0.5`:

```
if c_lock < 0.5:            DT_ABSENT
else if m0 != m1:           m1 < m0 ? DT_TRUE : DT_FALSE
else if c_invalid > 0.5:    DT_INVALID     // unrecoverable; group was poison
else:                       DT_ERASURE     // unrecoverable on a tracked stream
```

The recoverable branch is tested **before** `c_invalid` (a strict
"invalid-first" reading would destroy clean bits contaminated downstream of a
poison; the tie test is what isolates the originating poisoned step, per the
encoder property of §3.4). A single erasure is corrected by surrounding parity
(`m0 ≠ m1`); a sustained erasure burst drives the forced costs together and
reads as lost.

---

## 7. `vindel` — drift-tolerant hard-decision Viterbi

The full §4 machinery with backpointer traceback and hard output only. Channel
model: substitutions, erasures, unbiased insertions/deletions (no overwrites,
no `DT_INVALID` distinction).

### 7.1 Boundary conventions

Public boundary speaks `dt_bit`; the engine works in `0 / 1 / 0xFF`
(`VIN_ERASURE = 0xFF`). Ingest: `DT_IS_BIT(s) ? DT_BIT(s) : 0xFF` — i.e.
**`DT_INVALID` and `DT_ERASURE` are both treated as lost bits**. Output:
`bit ? DT_TRUE : DT_FALSE` (plus an optional parallel per-bit
`lock_probability` float array on the streaming call).

### 7.2 Parameters, costs, lock anchors (**NORMATIVE**)

Validation: `decision_depth ≥ 1`; `max_drift ≥ 0`; `0 < p_sub < 1`;
`0 ≤ p_erase < 1`; `p_ins, p_del ≥ 0`; `p_ins + p_del < 1`; and if
`max_drift > 0`, both `p_ins > 0` and `p_del > 0`.

```
cost_match = −log((1 − p_erase)(1 − p_sub))
cost_miss  = −log((1 − p_erase) · p_sub)
cost_erase = −log(p_erase)                 // +INF when 0 (never read)
cost_keep  = −log(1 − p_ins − p_del)
cost_ins   = −log(p_ins)
cost_del   = −log(p_del)
```

Match tables (§4.4) collapse to: erasure → `keep + cost_erase` on both;
boolean v → `keep + (v == expected ? cost_match : cost_miss)`; single
`cost_ins` everywhere. Lock anchors as §6.3 but with `cost_keep` added inside
the per-bit parenthesis:

```
misfit_lock = p_sub;  misfit_unlock = 0.5·(p_sub + 0.5)
expected_lock   = n·(cost_keep + erase_term + kept·((1−misfit_lock)·cost_match + misfit_lock·cost_miss))
expected_unlock = n·(cost_keep + erase_term + kept·((1−misfit_unlock)·cost_match + misfit_unlock·cost_miss))
```

### 7.3 Backpointers and traceback (**NORMATIVE**)

Per destination node, a packed 32-bit backpointer
`bit:1 | prev_drift_index:15 | prev_state:16` recorded by the strict-`<`
scatter (§4.7 order), in a ring of `decision_depth` layers of
`n_states · drift_width` entries. Guard at init:
`n_states − 1 ≤ 0xFFFF` and `drift_width − 1 ≤ 0x7FFF`.

Traceback from the frontier node to target step T: at step i, read the entry
at the current node in layer `i % decision_depth`; if `i == T`, the entry's
bit is the decision; otherwise translate the predecessor's drift across step
i's re-anchor:

```
prev_drift_index = entry.drift + shift[i % decision_depth]
node = entry.state * drift_width + prev_drift_index
```

(In-bounds by construction: `reanchor_metric` emptied the slot that would
translate out, so it is never a recorded predecessor.) Traceback never reaches
deeper than `decision_depth` retained layers.

### 7.4 Cadence and the no-drift fast path

- Look-ahead gate (non-draining): step only when
  `received_length ≥ read_base + n + max_drift + 1` (the +1 covers a
  forward re-anchor). Draining: `received_length − read_base ≥ n`.
- Before each step that would overwrite ring layer
  `steps − decision_depth`, emit that step's decision (as `viterbi` §5.3),
  with the current `lock_estimate` written to `lock_out` if provided.
- **Fast path (NORMATIVE for equivalence):** when `max_drift == 0` and
  `cost_ins == cost_del == +INF`, the alignment is forced diagonal and the
  branch cost is `n·cost_keep + Σ per-bit match cost` — a plain butterfly with
  no DP. This must yield exactly the general path's `T[n][n]` in the same
  (state, bit) order, so decoded output is identical.
- **Flush:** run the normal cadence on remaining whole groups, then trace the
  final `decided < steps` tail from the final frontier.

---

## 8. `hybrid` — drift-tolerant hard+soft decoder (rich channel)

The §4 machinery with the **rich channel model** (value-biased insertions,
overwrites, poison round-trip) and a **batched forward–backward soft output**
instead of backpointer traceback. Populates `c_true/c_false/c_lost/c_invalid/
c_lock`; the wrapper derives `c_absent = 1 − c_lock` (no deletion marginal —
that is `maxir`'s extra).

### 8.1 Parameters and validation (**NORMATIVE**)

Fields: `decision_depth`, `max_drift`, `p_flip`, `p_ins_true`, `p_ins_false`,
`p_ins_erase`, `p_del`, `p_ovr_true`, `p_ovr_false`, `p_ovr_erase`.
Validation: `decision_depth ≥ 1`; `max_drift ≥ 0`; `0 < p_flip < 1`; all
rates ≥ 0; `p_ovr = Σ p_ovr_* < 1`; `p_ins = Σ p_ins_*`,
`p_ins + p_del < 1`; if `max_drift > 0` then `p_ins > 0` and `p_del > 0`.

### 8.2 Channel costs and lock anchors (**NORMATIVE**)

A coded bit is overwritten with a fixed `DT_TRUE`/`DT_FALSE`/`DT_ERASURE`
(probs `p_ovr_*`, regardless of what was sent) — else transmitted
(`pn = 1 − p_ovr`) and flipped with `p_flip`. `p_ovr_erase` doubles as the
plain erasure rate.

```
cost_bit[0][0] = −log(p_ovr_false + pn·(1 − p_flip))
cost_bit[0][1] = −log(p_ovr_true  + pn·p_flip)
cost_bit[1][0] = −log(p_ovr_false + pn·p_flip)
cost_bit[1][1] = −log(p_ovr_true  + pn·(1 − p_flip))
cost_erase = −log(p_ovr_erase)                 // +INF when 0 (never read)
cost_keep  = −log(1 − p_ins − p_del)
cost_ins_t = −log(2 · p_ins_true)              // consume a received 1
cost_ins_f = −log(2 · p_ins_false)             // consume a received 0
cost_ins_e = −log(p_ins_erase)                 // consume an erasure
cost_del   = −log(p_del)
```

The ×2 on data insertions scores an inserted bit's value against a uniform 0/1
prior: an evenly split rate gives `−log(p_ins)` overall (the same realignment
eagerness as one combined rate), tilting only when the true/false rates
differ; an inserted erasure carries no value and is taken at its own rate.

Lock anchors:

```
avg_match = 0.5·(cost_bit[0][0] + cost_bit[1][1])
avg_miss  = 0.5·(cost_bit[0][1] + cost_bit[1][0])
misfit_lock   = (pn·p_flip + 0.5·(p_ovr_true + p_ovr_false)) / (1 − p_ovr_erase)
misfit_unlock = 0.5·(misfit_lock + 0.5)
erase_term = p_ovr_erase > 0 ? p_ovr_erase·cost_erase : 0
kept = 1 − p_ovr_erase
expected_lock   = n·(cost_keep + erase_term + kept·((1−misfit_lock)·avg_match + misfit_lock·avg_miss))
expected_unlock = n·(cost_keep + erase_term + kept·((1−misfit_unlock)·avg_match + misfit_unlock·avg_miss))
```

### 8.3 Sizing (**NORMATIVE**)

`ring_len = 2·decision_depth + 2`; `reacquire_after = 2·decision_depth`.
Rings, all indexed `step % ring_len`: `alpha_ring` (state metric entering each
step, snapshotted post-re-anchor / pre-scatter), `branch_ring` (a full
`align_shared` copy per step — `n_patterns · drift_width · stride` floats,
`stride = n + 2·max_drift + 1`), `shift`, `inv_ring` (count of
`is_invalid_sym` symbols among the step's **zero-drift** group of n),
`smoothed_ring` (see below).

### 8.4 Step ordering (**NORMATIVE**)

Per step: `pick_shift` → re-anchor (+ `read_base += sigma`) → record
`shift[slot]` → (unless no-drift fast path) `align_precompute` →
**snapshot** alpha, branch rows, invalid count → forward pass (general or
no-drift butterfly, condition: `max_drift == 0` and all three `cost_ins_*`
and `cost_del` are `+INF`; the no-drift snapshot computes per-pattern group
costs directly into the final-row cell `bslot[p·stride + n]`) → `steps++`,
`read_base += n` → **`smoothed_ring[steps % ring_len] = smoothed_cost`**
(indexed by the *new frontier*, i.e. post-increment) → re-acquisition rule.

### 8.5 Batched soft output (**NORMATIVE**)

`run()` alternates: (a) step forward while `steps − decided < 2·decision_depth`
and input allows (same look-ahead gates as §7.4); (b) compute
`horizon = draining ? steps : steps − decision_depth`,
`n_emit = min(horizon − decided, caller headroom)`; if > 0, one backward sweep
(`soft_batch`) serves all `n_emit` targets `[decided, decided + n_emit)`;
`decided += n_emit`.

`soft_batch(base, n_emit)`:

- Seed `beta_next[node] = (metric[node] == INF) ? INF : 0` at the frontier.
- For `t = steps − 1` down to `base`: `sigma` = `shift[(t+1) % ring_len]`
  (0 when `t + 1 == steps`). For each source `(state, di)` with finite
  `alpha_t`, for each bit, over the folded ending-drift range
  `lo = max(max(0, di − n), sigma)`, `hi = min(drift_width, drift_width + sigma)`:

  ```
  cand = branch_row[n + (nd − di)] + beta_next[next_state·drift_width + (nd − sigma)]
  best_edge = min over nd
  ```

  `beta_cur[state,di] = min over bits of best_edge`; when `t < base + n_emit`
  and `best_edge` finite, fold `alpha + best_edge` into `m0`/`m1`. Normalize
  `beta_cur`; swap. (`beta[base]` itself is never consumed.)
- Emission for target t: `c_invalid = inv_ring[t % ring_len] / n`; the lock is
  sampled at the target's **own decision time** — `smoothed_ring[s % ring_len]`
  with `s = min(t + decision_depth, steps)` (the clamp covers the reduced-depth
  flush tail).

### 8.6 Projection (`finalize_soft`) (**NORMATIVE**)

```
mmin = min(m0, m1)
c_true  = (m1 == INF) ? 0 : exp(mmin − m1)
c_false = (m0 == INF) ? 0 : exp(mmin − m0)
c_lost  = 1 − |c_true − c_false|
c_lock  = lock_from(smoothed sample)
```

`c_lost` is the *agreement* of the two value reads: high when the values are
indistinguishable — whether evidence is absent (both ≈ 1) or lock is lost on
random data — and 0 when one side is infeasible (determined, not lost). It is
deliberately **not** scaled by `c_lock` and stays high through a lock
collapse. Hard cascade:

```
if c_lock < 0.5:                          DT_ABSENT
else if c_lost >= c_true and c_lost >= c_false:
    c_invalid > 0.5 ? DT_INVALID : DT_ERASURE      // leads only on an exact m0 == m1 tie
else:                                     c_true >= c_false ? DT_TRUE : DT_FALSE
```

(Note `c_true >= c_false → DT_TRUE` on the value branch; the tie case is
unreachable there since a tie makes `c_lost` lead.)


---

## 9. `maxir` — drift-tolerant max-log-MAP (the strongest decoder)

Everything `hybrid` models, decoded by a full forward–backward recursion over
the (state × drift) super-trellis with **snapshotted gammas** and an
additional **deletion marginal** (`c_absent` becomes a real per-step read, not
`1 − c_lock`). Independent implementation from `bcjr`.

### 9.1 Parameters, channel, lock anchors

**Identical to `hybrid`** (§8.1, §8.2) — same parameter struct shape, same
validation, same cost derivations, same lock anchors.

### 9.2 Sizing and rings (**NORMATIVE**)

`batch = decision_depth`; `ring_len = 2·decision_depth + 2`;
`reacquire_after = 2·decision_depth`. Rings indexed `step % ring_len`:
`alpha_ring` (`count = n_states·drift_width` floats/step), `branch_ring`
(full `align_shared` per step), `shift`, `lock_ring`
(**trailing** smoothed cost *after* the step's forward pass — note the
indexing difference from `hybrid`'s frontier-indexed `smoothed_ring`),
`inv_ring` (zero-drift-group invalid count, snapshotted before the pass).
Output FIFO of `ring_len` (symbol, detail) pairs.

### 9.3 Step ordering (**NORMATIVE**)

`pick_shift` → re-anchor + `read_base += sigma` → `shift[slot] = sigma` →
`align_precompute` → snapshot alpha (pre-pass metric), branch rows, invalid
count → forward pass (min-plus scatter, §4.7; no backpointers; **no no-drift
fast path** — the general path always runs) → `lock_ring[slot] = smoothed_cost`
→ re-acquisition rule → `steps++`, `read_base += n`.

### 9.4 Cadence (**NORMATIVE**)

FIFO-drain-first `produce` loop, exactly as `bcjr` §6.4: when the FIFO is
empty, step forward while possible (gates as §7.4); once
`steps − committed ≥ decision_depth + batch`, sweep with
`emit_hi = committed + batch − 1`; when draining, a final sweep with
`emit_hi = steps − 1`.

### 9.5 Backward sweep with drift (**NORMATIVE**)

Seed `beta_tilde[node] = (metric[node] == INF) ? INF : 0`. For
`t = steps − 1` down to `committed`, with `sigma = shift[(t+1) % ring_len]`
(0 at the frontier): the same folded-range min reduction as `hybrid` §8.5
(gammas from `branch_ring[t % ring_len]`, betas indexed `nd − sigma`),
accumulating `m0`/`m1` on emit steps — **plus the deletion marginal**:

```
// inside the per-(state, di, bit) edge loop, on emit steps only:
if lo < hi and lo < di:
    tot_del = alpha + branch_row[n + (lo − di)] + beta_next[next·drift_width + (lo − sigma)]
    m_absent = min(m_absent, tot_del)
```

`m_absent` is the cheapest complete path whose step-t edge **maximally
deletes** this group — consumes the fewest received bits (`nd = lo`, consumed
`= n + (lo − di)`); the `lo < di` gate restricts it to genuine deletions
(consumed < n). With no drift no deletion branch exists and `m_absent` stays
`INF` → `c_absent = 0`. Normalize `beta_cur` each step; swap; emit for
`t ∈ [committed, emit_hi]` in descending order into FIFO slots
`t − committed` (so the FIFO reads out in ascending step order).

### 9.6 Projection (`finalize_emit`) (**NORMATIVE**)

`c_lock = lock_from(lock_ring[t % ring_len])` — sampled at the bit's **own
step**. (Rationale, normative for behavior: `lock_ring` is a *causal* EWMA;
reading it ahead at `t + k` leaks a future loss backward onto still-recoverable
bits before the loss — empirically ~k bits destroyed, worst at
`t + decision_depth`. The own-step sample's only artifact is a benign recovery
lag *after* a loss, where the decoder is genuinely re-acquiring. This is a
deliberate divergence from `hybrid`.)

`c_invalid = inv_ring[t % ring_len] / n`.

- `mmin == INF`: `c_true = c_false = c_lost = 0`, **`c_absent = 1`**
  (no surviving path — contrast `bcjr`'s `1 − c_lock`), symbol `DT_ABSENT`.
- Else: `c_true = exp(mmin − m1)`, `c_false = exp(mmin − m0)`,
  `c_lost = min(c_true, c_false)`,
  `c_absent = (m_absent == INF) ? 0 : exp(mmin − m_absent)` — the deletion
  analog of `c_true`: → 1 when routing around this group is as cheap as
  placing a value, → 0 when no deletion is viable.
- Hard cascade identical to `bcjr` §6.5 (`ABSENT` on low lock; value on
  `m0 ≠ m1`; tie splits `DT_INVALID` above `c_invalid > 0.5` else
  `DT_ERASURE`).

---

## 10. `detect_clean` — blind code-presence detector (GF(2) rank)

A **meta-codec**: it does not decode. Given an arbitrary symbol stream — no
code, rate, generators, or alignment known — it emits one record per input
position with two **independent** consistencies:

- `c_lost` (→ soft `c_erasure`): consistency with "a convolutional code IS
  present here";
- `c_absent`: consistency with "no code / random".

Each is a goodness-of-fit; they need not sum to 1, and the no-evidence state
is `(1, 1)` — with nothing to judge, neither hypothesis is contradicted.

### 10.1 Principle

A convolutional code is linear and time-invariant, so width-W windows of its
output, **phase-aligned** (taken at stride s = block size n), lie in a proper
GF(2) subspace: a matrix of such rows is rank-**deficient**, while random rows
are full-rank. n is unknown → sweep candidate strides. Windows **slide**: any
row spanning an indel or straying across a code/random boundary is independent
and erases the deficiency, so a matrix is deficient only when all its rows lie
inside one indel-free aligned run — sparse indels only kill the windows that
span them, and localization stays sharp (a straddling window reads d = 0).

### 10.2 Geometry (**NORMATIVE constants**)

```
W = 32          // GF(2) row width, bits (must exceed the parity span ~2K)
SMAX = 6        // strides swept: s = 2..6 (block sizes n in 2..6)
MARGIN = 18     // extra rows per window beyond W
STEP = 32       // window slide granularity, bits
LWIN(s) = s·(W + MARGIN) + W      // window length at stride s
MAXL = LWIN(6) = 332;  MINL = LWIN(2) = 132
```

Every stride yields the same row count `N = W + MARGIN + 1 = 51`, hence the
same random rejection ≈ `2^−(N−W)`.

### 10.3 Ingest (**NORMATIVE**)

Each input symbol maps to `0`/`1` (`DT_IS_BIT` → `DT_BIT`), or sentinel
`NOTBIT = 2` for an **unbound** non-bit (erasure/absent/none — neutral), or
`INVAL = 3` for a **bound** non-boolean (`DT_INVALID`). Parallel per-position
pools, max-pooled over covering windows: `dmax` (best structural deficiency;
−1 = never covered by a usable-row window), `cmax` (largest usable row count
of any covering window), `imax` (worst invalid-placement penalty; 0 = none).

### 10.4 Rank computation (**NORMATIVE**)

`gf2_rank(win, count, stride, *nrows)`: rows are the W-bit slices at offsets
0, s, 2s, … with `start + W ≤ count`. A row containing any sentinel (> 1) is a
**don't-know and is dropped** (never coerced to 0, which would fake the
all-zeros maximally-deficient row); `*nrows` counts usable rows. Online
Gaussian elimination against an MSB-indexed pivot table of W `uint64_t`
pivots: pack the row MSB-first into a 64-bit word `v`; while `v ≠ 0`, let
`b = msb(v)`; if `pivot[b]` exists, `v ^= pivot[b]`, else `pivot[b] = v`,
`rank++`, break.

Per (window, stride): `fill = min(n_eff, W)`;
`def = max(0, fill − rank)`. Fold `def` into `dmax` and `n_eff` into `cmax`
(max) over **all L positions the window covers** — folding `def ≥ 0` also
flips "never covered" (−1) to covered. Skip strides whose window doesn't fit;
skip folds when `n_eff ≤ 0` (all rows don't-know: no evidence).

### 10.5 Invalid-placement evidence (**NORMATIVE**)

Encoders emit invalids only in generator-shaped clusters/runs, never as
singletons, and every run advances the trellis by its length. Two
generator-agnostic signatures of an **un-encodable** placement, summed into
penalty units:

- `singles`: number of length-1 `INVAL` runs;
- `max(0, distinct_run_lengths − 1)`: runs of differing length impose
  independent state offsets one code must satisfy jointly.

A single run, or equal-length runs, contradict nothing → 0 units. `NOTBIT`
never counts. Track at most `MAXRUNS = 64` runs; cap units at
`MAXUNITS = 32`. Scanned once per window start over the **widest** window
length that fits (the pattern is stride-independent), max-pooled into `imax`
over the positions it covers.

### 10.6 Detectability (**NORMATIVE**)

From the same channel-model parameter struct the real decoders take (used for
calibration only; `decision_depth`/`max_drift` accepted for interface
uniformity and ignored):

```
p_corrupt = p_ovr + (1 − p_ovr)·p_flip, clamped to ≤ 0.999;  p_ovr = Σ p_ovr_*
detectability = (1 − p_corrupt)^W      // 1.0 when p_corrupt ≤ 0
```

— the chance a width-W row survives expected flips intact, i.e. how far a
full-rank window can rule a code OUT. Indels are deliberately **excluded**
(the sliding method tolerates them).

### 10.7 Verdict (**NORMATIVE**)

Per position, from pooled `(d = dmax, cov = cmax, iunits = imax)` with
`pow2_neg(x) = 2^−x` (1 for x ≤ 0, 0 for x ≥ 64):

```
if d < 0:      c_lost = 1;              c_absent = 1        // never covered
else if d ≥ 1: c_lost = 1;              c_absent = 2^−d     // structure found
else:          // d == 0, no structure
    margin   = cov − W
    fillconf = margin ≥ 0 ? 1 − 2^−(margin+1) : 0
    c_lost   = 1 − detectability·fillconf;  c_absent = 1
// two-sided invalid evidence, applied last:
if iunits > 0:
    f = 2^−(2·iunits)                    // INV_BITS = 2 per unit
    c_lost   *= f
    c_absent  = 1 − (1 − c_absent)·f
```

Rationale encoded here: deficiency contradicts random regardless of channel
(noise erodes structure, never manufactures it); full rank contradicts a code
only insofar as the fill is confirmed (margin) *and* the channel is clean
enough that flips could not have filled it; the `c_absent` lift also undoes
spurious deficiency that dropped invalid rows can leave on a thinned random
window (scattered invalids must never read *more* code-like).

### 10.8 Cadence (**NORMATIVE**)

`next_start` walks multiples of `STEP` from 0. On feed: while
`next_start + MAXL ≤ buffered_end`, process the windows starting there (all
strides that fit) and `next_start += STEP`; then emit finalized records for
every position `< next_start` (their coverage is settled). On flush: continue
full-fit starts, then keep processing partial starts while
`next_start + MINL ≤ end` (each start scores only the strides whose window
fits), then emit everything. Output trails input by ≤ MAXL positions.

Limitations to preserve/document: flips are NOT tolerated (one flip is an
independent row in every window covering it) — this is the clean/very-low-noise
detector, holding to ~1% flips, ~2–3% indels; rank deficiency senses linear
structure generally (a block linear code or LFSR scrambler also registers);
block sizes n > 6 are outside the stride sweep.

---

## 11. `detect_noisy` — blind code-presence detector (FWHT parity bias)

Same contract, sliding-window plumbing, ingest sentinels, invalid-placement
evidence, and two-consistency output as `detect_clean`; the statistic differs.

### 11.1 Principle

A linear code has low-weight parity checks c with c·(aligned window) = 0.
Under per-bit flip rate p a weight-w check retains **bias**
`β(c) = |E[(−1)^{c·row}]| ≈ (1 − 2p)^w > 0`, while random data gives β ≈ 0
for every c. A flip *shrinks* the bias instead of destroying the check
(contrast the rank method), so the statistic degrades gracefully with flips,
with indels (post-slip rows fall out of phase and merely stop contributing),
and with combinations. The max bias over all `2^Lc` candidate checks at once
is exactly a Walsh–Hadamard transform of the row-slice histogram.

### 11.2 Geometry (**NORMATIVE constants**)

```
LC = 14                   // check span / transform order (2^14 = 16384-entry histogram, ~64 KB int32)
SMAX = 6                  // strides 2..6
L = 1200                  // window length; STEP = 300; MINL = 256 (shortest scored tail)
K_LOST = 2.0              // excess at which c_lost saturates
WREF = 7                  // representative check weight for detectability
FLOOR_C = 2·LC·ln2        // numerator of f0²·N
UNCOVERED = −1e30         // per-position emax sentinel
```

### 11.3 Statistic (**NORMATIVE**)

`window_max_bias(win, span, stride)`: zero the `2^LC` histogram; for each row
start `t = 0, s, 2s, …` with `t + LC ≤ span`, pack the LC-bit slice MSB-first;
skip the slice if it contains any sentinel (> 1) — a don't-know, never coerced
(coercion to 0 would fake an all-zeros delta whose transform is flat and reads
as maximal bias); else `acc[v]++`, `n++`. In-place FWHT (the standard
butterfly: for `len = 1; len < M; len <<= 1`, pairs `(a, b) → (a+b, a−b)`).
Return `max_{c ≠ 0} |acc[c]| / n` (skip c = 0, the DC term ≡ n), and n.

Random floor from the **actual** row count (short tails get their higher
floor): `f0(n) = sqrt(2·LC·ln2 / n)` (computed as `exp(0.5·log(FLOOR_C/n))`).
Per-window evidence: `excess = max over strides s of (β_s / f0(n_s) − 1)`
(strides with n < 1 skipped) — ≈ 0 for random, large for an aligned code.
Fold the window's `excess` (max) into per-position `emax` and its
`invalid_units` (same algorithm as §10.5, over the full window span) into
`imax`.

### 11.4 Detectability (**NORMATIVE**)

```
p_corrupt = p_ovr + (1 − p_ovr)·p_flip
one_minus_2p = 1 − 2·p_corrupt
detectability = 1 if one_minus_2p ≥ 1; 0 if ≤ 0; else (one_minus_2p)^WREF
```

(≥ 50% corruption erases all bias: a code can never be ruled out.) Indels
again excluded.

### 11.5 Verdict (**NORMATIVE**)

```
if emax == UNCOVERED:  (c_lost, c_absent) = (1, 1)
else:
    p_ev    = clamp(excess / K_LOST, 0, 1)
    c_lost  = 1 − detectability·(1 − p_ev)     // a peak fits a code outright;
                                               // no peak rules one out only per detectability
    c_absent = clamp(1 − excess, 0, 1)         // a peak contradicts random; its absence never does;
                                               // an observed peak is real whatever the channel,
                                               // so detectability does NOT scale this axis
// then the identical two-sided invalid adjustment of §10.7
```

### 11.6 Cadence

As §10.8 with (L, STEP, MINL) = (1200, 300, 256); on flush, partial tail
windows are scored over `span = end − start` (fewer rows → higher floor →
conservative verdict).

Envelope to preserve/document: reliable to ~5% flips (marginal ~8%), ~2–3%
indels, light–moderate combinations; heavy simultaneous flips+indels are out
of reach (the underlying LPN + synchronization problem); checks longer than
LC bits or block sizes n > 6 are not covered; cost is one histogram + FWHT per
(window, stride) — one to two orders more work per bit than `detect_clean`.

---

## 12. Streaming integration and soft-field mapping

Every decoder is wrapped as an abstract `dt_stream_decoder` (hard) and/or
`dt_stream_soft_decoder`, a small vtable `{begin, decode, finalize, data}`
driven `begin → decode (repeat) → finalize (repeat until 0)`:

- `begin(src, src_len)` — all cc codecs consume no preamble; returns 0.
- `decode(dst, dst_len, src, src_len)` — feeds `src` into the engine and
  collects up to `dst_len` outputs; returns the count written. Input must be
  handed to the engine even when `dst_len == 0` (a feed-only "pump" call).
  Soft wrappers pull engine details through a fixed 64-element stack chunk and
  translate per-record; on multi-chunk collects, `src` is fed only on the
  first engine call (subsequent calls pass NULL/0 to drain).
- `finalize(dst, dst_len)` — drains via the engine's flush; call until it
  returns 0.

**Soft-field translation (NORMATIVE):**

| engine field | soft field |
|---|---|
| `c_false`, `c_true` | same |
| `c_lost` | `c_erasure` |
| `c_invalid` | `c_invalid` (`hybrid`, `maxir`, `bcjr`) |
| `c_absent` | `maxir`/`bcjr`: engine value; **`hybrid`: `1 − c_lock`** |
| `c_lock` | `c_locked` |

`detect_*` wrappers fill only `c_erasure = c_lost` and `c_absent`; all other
fields 0. `vindel`'s streaming call optionally returns a per-bit
`lock_probability` float array instead.

Warm-up: the first ~`decision_depth` outputs of any blind-acquisition decoder
are unreliable (`c_locked` low); output trails input by `decision_depth`
steps (`viterbi`, `vindel`) or up to `decision_depth + batch` (`bcjr`,
`hybrid`, `maxir`), or by one longest window (`detect_*`).

---

## 13. Numeric and portability requirements

- Costs are non-negative by construction (all probabilities < 1), so metric
  normalization variants ("subtract when min > 0" vs "return 0 when min ≤ 0")
  are equivalent in practice; implement either, but always leave `+INF` nodes
  untouched.
- `exp` appears only as `exp(mmin − m) with mmin ≤ m` (arguments ≤ 0, results
  in [0, 1]) and in the detectors' `2^−x` / power helpers; a monotone,
  reasonably accurate single-precision proxy suffices for soft-value
  equivalence.
- Winner-reads-1 exactness: `exp(0) = 1` must hold exactly so the argmax value
  hypothesis reads consistency 1.
- No dependence on libm at link time is required by the reference (freestanding
  core); an implementation may use libm freely.
- Determinism: hard decisions are exactly reproducible given the NORMATIVE
  iteration orders and strict-`<` tie-breaks; soft values match up to
  float-associativity differences in the alignment DP and min reductions
  (the reference keeps a fixed order there too, so exact match is attainable).
- Memory (dominant terms): drift decoders hold
  `O(ring · n_states · drift_width)` alpha floats and
  `O(ring · n_patterns · drift_width · stride)` branch floats
  (`maxir`/`hybrid`); `vindel` holds `O(decision_depth · n_states ·
  drift_width)` 32-bit backpointers; `bcjr` holds `O(ring · n_states)` floats
  and re-reads the received buffer; `detect_clean` a few KB; `detect_noisy`
  one ~64 KB `int32` histogram.

## 14. Conformance checklist

An implementation is equivalent when all of the following hold:

1. Encoder: for every code and input over {T, F, E, I}, coded output matches
   symbol-for-symbol, including poison/erasure tap propagation, the
   INVALID > ERASURE > clean precedence, and the K−1-group flush.
2. `viterbi`: bit-identical output on any input, including erasure neutrality
   and end-of-stream reduced-depth flush.
3. `vindel`: bit-identical output for any (params, input), including across
   re-anchors, re-acquisition events, and the no-drift fast path; lock
   probabilities match numerically.
4. `bcjr`/`hybrid`/`maxir`: hard symbols bit-identical; soft records match to
   float tolerance; the three lock-sampling policies (bcjr: own-step trailing;
   hybrid: `t + decision_depth` frontier-indexed, clamped; maxir: own-step
   trailing) preserved; `maxir`'s deletion marginal and `mmin == INF` handling
   (`c_absent = 1`) preserved; `hybrid`'s `c_absent = 1 − c_lock` preserved.
5. `detect_clean`/`detect_noisy`: per-position records match to float
   tolerance for any input, including sentinel-row dropping, per-position max
   pooling, partial-window flush behavior, and the two-sided invalid
   adjustment.
6. All parameter-validation rejections (NULL/range/probability-sum rules)
   match.
