# drifty — Data-Flow Semantics

This document describes the meaning of the symbol streams that flow through
drifty, component by component, in terms of the `DT_` symbol alphabet. It is
deliberately abstract: no data structures, no algorithms, no codec internals —
only what each component *consumes*, what it *produces*, and what those symbols
*mean* at each interface.

The pipeline is:

```
source data ──▶ encoder ──▶ channel ──▶ decoder ──▶ recovered data ──▶ consumer
            (transmit)   (transmit)   (transmit)    (output)        (output)
```

The bracketed label under each arrow is the *domain* of the alphabet crossing
that boundary (defined in §1).

---

## 1. The symbol alphabet

The whole system speaks one vocabulary. Every interface — source, coded,
received, recovered — is a stream of per-*position* symbols. The position is the
native unit; a *value* is a property a position may or may not carry. This
position-before-value stance is the reason the alphabet is richer than `{0, 1}`.

The six symbols:

| Symbol        | Meaning                                                                                 |
|---------------|-----------------------------------------------------------------------------------------|
| `DT_TRUE`     | A bound boolean: value 1.                                                                |
| `DT_FALSE`    | A bound boolean: value 0.                                                                |
| `DT_ERASURE`  | A present position whose value is **unspecified** (the "neither" cell).                  |
| `DT_INVALID`  | A present position **specified as neither true nor false** — a deliberate non-value (the "both" cell). |
| `DT_ABSENT`   | A position inferred to have **not been delivered**: the deletion verdict. Output-only.  |
| `DT_NONE`     | The **absence of any symbol**. Belongs to no data flow; exists only so that zeroed or uninitialized storage is detectably *not* a symbol. |

### Domains

Two domains partition where each symbol may legitimately appear:

- **Transmit domain** — `{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID}`. These are
  the symbols that can be *sent*: what an encoder accepts, and what travels on
  the channel.
- **Output domain** — `{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID, DT_ABSENT}`.
  These are the symbols a decoder may *emit*. It adds `DT_ABSENT`, which names a
  position deleted in transit and therefore one that could never itself have been
  transmitted.

`DT_NONE` is in **neither** domain.

### Structure within the alphabet

- The **bound** symbols `{DT_TRUE, DT_FALSE, DT_INVALID}` commit to a definite
  content — a value, or a definite non-value.
- `DT_ERASURE` is the **unbound** symbol, present in both domains: it commits to a
  *position* but not to a *content*.
- `DT_ABSENT` denies the position itself, and exists only on output.

### One symbol, one invariant — not one cause

A symbol that appears in both domains means the same *predicate* on both sides,
even when the *reason* differs:

- `DT_ERASURE` on the way **in** means "value unspecified because the source
  declined to give one" — **don't-care**.
- `DT_ERASURE` on the way **out** means "value unspecified because it could not be
  recovered" — **don't-know**.

These two readings **cannot be exchanged across the channel** (the source's reason
and the sink's reason are different facts), but they name the same predicate —
*value unspecified* — which is why a single symbol carries both.

---

## 2. Source data → encoder

**Input alphabet:** transmit domain `{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID}`,
one symbol per information position.

- `DT_TRUE` / `DT_FALSE` — the ordinary case: a bit to be protected.
- `DT_ERASURE` — a **don't-care** position. The source declines to fix a value and
  defers it to the channel, which is free to concretize it.
- `DT_INVALID` — a deliberately non-boolean position the source wishes to transmit
  *as* a non-value.

`DT_ABSENT` and `DT_NONE` are **not** valid source inputs: `DT_ABSENT` is a
receiver inference, and `DT_NONE` is not a symbol.

---

## 3. Encoder

**Consumes:** one transmit-domain information symbol per step.

**Produces:** a coded *group* of transmit-domain symbols per step — the code's
output positions for that step.

**What it does:** expands the information stream into a redundant coded stream by
the convolutional code, carrying each position's semantics through:

- A **boolean** input (`DT_TRUE`/`DT_FALSE`) yields a coded group of bound
  booleans determined by the code.
- An **`DT_INVALID`** input has no value to protect, so the coded positions that
  would have carried that value are emitted as `DT_INVALID` — *structural poison*
  marking "there was no value here." The poison lands exactly on the coded
  positions that depended on the missing value, and nowhere else.
- An **`DT_ERASURE`** input is unbound, so it is emitted as `DT_ERASURE` into the
  coded stream and *deferred* — left for the channel to resolve.

The output is a transmit-domain stream at coded granularity.

> **Encoder output is not decoder input.** The channel lies between them. Any
> meaning that depends on *which side* you are on — most importantly the
> don't-care vs don't-know readings of `DT_ERASURE` — must not be carried across as
> though it were the same fact.

---

## 4. Channel

**Consumes:** the coded transmit-domain stream, one symbol per coded position, on
a **known** grid.

**Produces:** a received transmit-domain stream on an **unknown** grid — the same
alphabet `{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID}`, but with the
position-to-position correspondence broken.

**What it does:** applies a memoryless, asymmetric corruption with effects on two
distinct axes.

**Value axis** (length-preserving — one sent position ↦ one received position):

- **Substitution** — a bound boolean arrives flipped (`DT_TRUE` ↔ `DT_FALSE`).
- **Overwrite** — a position is forced to a fixed content (`DT_TRUE`, `DT_FALSE`,
  or `DT_ERASURE`) regardless of what was sent. Asymmetric: the forcing may favor
  one content over another.
- **Erasure** — a position's value fails to survive and arrives as `DT_ERASURE`.
  This is the **don't-know** erasure; it is the *same symbol* the encoder uses for
  don't-care, justified only by the shared "unspecified" invariant.

**Alignment axis** (length-changing — the source of broken correspondence):

- **Insertion** — a spurious position enters the stream that was never sent,
  value-tagged as a spurious `DT_TRUE`, `DT_FALSE`, or `DT_ERASURE`. The received
  stream **grows**.
- **Deletion** — a sent position yields nothing received. The received stream
  **shrinks**. There is **no symbol** for a deletion on the wire: it is a *gap* —
  the absence of a position — not a delivered `DT_ABSENT`.

`DT_INVALID` poison passes through as an ordinary transmit-domain symbol.
`DT_ABSENT` and `DT_NONE` **never appear on the channel**: the channel makes gaps
but does not deliver the deletion *verdict*, and it never manufactures the
no-symbol sentinel.

The net misalignment between sent and received position indices —
**insertions minus deletions** — is the **drift**. That this drift is unknown and
time-varying is the entire premise of the decoder.

---

## 5. Decoder

**Consumes:** the received transmit-domain stream on an unknown, drifting grid —
with extra positions (insertions) and missing positions (deletions).

**Produces:** a recovered stream in the **output domain**, indexed by *transmitted
information position* — one output symbol per information bit the source put in,
with position restored.

**What it does:** jointly resolves **where** the bits are (recovers alignment /
absorbs drift) and **what** they are (undoes the code), then reports a per-position
verdict.

For each restored information position it emits an output-domain symbol:

- `DT_TRUE` / `DT_FALSE` — the value was recovered.
- `DT_ERASURE` — the position is *tracked* (its place in the stream is known) but
  its value could not be recovered: the **don't-know** erasure.
- `DT_INVALID` — the position's coded support arrived as the encoder's poison, so
  the decoder reads it as a deliberate tie and reports the source's original
  non-value. `DT_INVALID` thus **round-trips**: a sent `DT_INVALID` is recovered as
  `DT_INVALID` (channel damage permitting) — unlike `DT_ERASURE`, whose meaning
  changes sides.
- `DT_ABSENT` — the decoder infers the position was deleted in transit (or that
  the stream is not synchronized there, so no value verdict is possible). This is
  the only output symbol that could never have been transmitted: it is the
  alignment-axis fact surfacing as a per-position verdict.

**Verdict ordering is recoverability-first.** A position whose value is
determinable resolves to `DT_TRUE`/`DT_FALSE` even when its surroundings were
damaged. Only a position with *no* recoverable value falls through to
`DT_INVALID` (if its support was poison) or `DT_ERASURE` (if its value was merely
lost). A position the receiver cannot place at all is `DT_ABSENT`.

**Soft output.** Each hard verdict is accompanied by a graded *consistency* over
the same output alphabet — how strongly the position reads as true, as false, as
lost (`DT_ERASURE`), as poison (`DT_INVALID`), and as absent (`DT_ABSENT`) — so a
downstream consumer can use soft information rather than the hard symbol alone.
The hard symbol is the argmax projection of that consistency.

**The two channel axes surface asymmetrically on output:**

- **Deletions are visible** — as `DT_ABSENT`: a position that exists in the output
  index but carries no value.
- **Insertions are invisible** — a spurious received position has no transmitted
  index, so it has no place in the output and simply disappears.

The output is therefore a clean, fixed grid of *information positions*, free of
insertions.

---

## 6. Recovered data → consumer (system boundary)

This is the inner→outer boundary — the edge of drifty proper, where the recovered
stream is handed to an outer code or final sink.

**Input alphabet:** output domain
`{DT_TRUE, DT_FALSE, DT_ERASURE, DT_INVALID, DT_ABSENT}`, one symbol per
information position, on a restored grid.

**What the symbols mean downstream:** because position is restored and insertions
are gone, a consumer sees only *value-axis* phenomena — values and gaps — on a
fixed grid:

- `DT_TRUE` / `DT_FALSE` — usable values.
- `DT_ERASURE` and `DT_ABSENT` — **erasure flags** for an errors-and-erasures
  outer decoder. `DT_ERASURE` is a value lost on a delivered position; `DT_ABSENT`
  is a position lost outright. Both tell the outer code "no value here — but do
  not treat this as a *wrong* value."
- `DT_INVALID` — a recovered deliberate non-value; the consumer handles it per its
  own contract (e.g. a structural marker, a forced tie, or a hard erasure).

The inner system's contract, stated as one sentence: **convert the alignment-axis
mess (drift, insertions, deletions) into value-axis facts (values plus erasures)
on a stable grid**, so that an outer, value-native code never has to reason about
position.

---

## 7. Invariants (stated once)

- **Domains.** Sent symbols are transmit-domain; emitted symbols are
  output-domain. The two differ only by `DT_ABSENT` (output-only) and by the two
  readings of `DT_ERASURE`. `DT_NONE` is in neither.
- **Encoder output ≠ decoder input.** The channel intervenes; side-dependent
  meaning must not be carried across.
- **`DT_ERASURE` is defined by invariant, not by cause.** One symbol, meaning
  *value unspecified*, with a don't-care reading on input and a don't-know reading
  on output.
- **`DT_INVALID` round-trips; `DT_ERASURE` does not.**
- **Deletion is visible (`DT_ABSENT`); insertion is invisible (no index).**
- **Drift = insertions − deletions** is the latent the inner stage exists to
  absorb.
- **`DT_NONE` is never a data symbol** — only a guard so that empty storage cannot
  be mistaken for content.
