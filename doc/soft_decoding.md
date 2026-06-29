# drifty — soft decoding

Three of the convolutional decoders have a **soft** front end that reports, per
recovered information position, a graded *consistency* for each output-domain
hypothesis instead of a single hard symbol: [`bcjr`](cc/bcjr.md) on a
flip/erasure channel, and [`hybrid`](cc/hybrid.md) and [`maxir`](cc/maxir.md) on a
drifting one. The Reed–Solomon block codec [`rs251`](bc/rs251.md) also has a
soft-input decoder. This page is about what the soft output *means*, how the three
decoders differ in producing it, and how to use it across the inner→outer boundary
— the practical payoff being that the soft inner decoders recover values the hard
decision discards.

All three now populate the **full** `dt_soft_bit` alphabet. (`hybrid` formerly left
`c_invalid` and `c_absent` at 0; it no longer does.) For the field definitions see
[Symbols](bit.md#the-soft-symbol-dt_soft_bit); the short version:

| Field | Hypothesis |
|-------|-----------|
| `c_false` / `c_true` | the position holds `DT_FALSE` / `DT_TRUE` |
| `c_erasure` | the value is unrecoverable on a tracked stream (`DT_ERASURE`) |
| `c_invalid` | the coded group was the encoder's deliberate-non-value poison (`DT_INVALID`) |
| `c_absent` | the position was deleted / not placed (`DT_ABSENT`) |
| `c_locked` | the decoder is tracking a valid stream of this code |

The fields are consistencies in `[0, 1]`, **not** a probability split — they need
not sum to 1 — and the hard symbol is their argmax under a recoverability-first
cascade (a value is resolved before falling through to erasure/invalid/absent).

## `c_absent`: a deletion marginal in `maxir`, a lock proxy in the others

The three decoders compute `c_absent` two different ways, and the difference
matters when an outer code consumes it.

- **`maxir`** computes a true **deletion marginal**: `c_absent = exp(mmin −
  m_absent)`, where `m_absent` is the cheapest complete path whose edge at this
  step maximally *deletes* the coded group (routes around it) rather than placing
  a value. So `c_absent → 1` when deleting the group is as cheap as decoding a
  value there, and `→ 0` when no deletion is viable. It is the deletion analog of
  `c_true`/`c_false`, computed in the same forward–backward pass, and is
  **independent of lock** — it can flag a single deleted position sharply while the
  decoder stays fully locked around it.
- **`hybrid`** and **`bcjr`** use the cheaper proxy `c_absent = 1 − c_locked`. For
  `bcjr` this is the only sensible notion — it models a flip/erasure channel with
  no drift axis, so the only way a position goes "absent" is a re-acquisition gap,
  i.e. lost lock. For `hybrid`, which *does* track drift, `1 − c_locked` is a
  deliberate approximation: it rises across any low-confidence stretch rather than
  pinpointing the deleted symbol.

Concretely, on a stream with one genuine in-window deletion, `maxir` reports
`c_absent ≈ 1.0` on the one or two symbols spanning the deletion (and still
recovers their values), while `hybrid` barely registers it — its lock holds
through the deletion, so `1 − c_locked` stays near 0. If you need a faithful
per-position "this was deleted" signal, that is `maxir`'s; `hybrid`'s `c_absent`
answers the coarser question "is the decoder unsure around here."

**What none of them flag: confident misalignment.** When net drift exceeds
`max_drift`, the decoder does not route around individual deletions — it loses the
phase and decodes *through* the stream at a wrong alignment, emitting a run of
confidently wrong values. That residue carries low `c_absent` (the decoder thinks
it is fine) in *every* decoder, `maxir` included, because it is not locally
deletion-shaped. A deletion marginal localizes deletions the decoder can place; it
cannot surface a global phase slip. This is the structural reason a per-symbol
reliability flag is not a complete substitute for staying within `max_drift`.

## `c_invalid`: the poison fraction

`c_invalid` is the fraction of the step's (zero-drift) coded group that arrived as
the encoder's `DT_INVALID` poison marker, snapshotted at forward time (exact when
local drift is 0). It rests on an encoder property: a `DT_INVALID` input poisons
exactly the coded bits that would carry its value, so a poisoned step has no clean
value evidence and decodes as a genuine `c_true == c_false` tie — which the hard
cascade then resolves to `DT_INVALID` (when the group was poison) rather than
`DT_ERASURE`. This is what lets `DT_INVALID` **round-trip** through the
full-alphabet decoders. On an ordinary channel that never carries poison,
`c_invalid` stays 0 everywhere.

## Using soft output with an outer code

The headline reason to run the soft front end is not the exotic fields — it is
that the soft decoder recovers values the **hard** decision throws away. The hard
cascade emits `DT_ABSENT`/`DT_ERASURE` over a low-lock stretch (e.g. an erasure
burst), but the convolutional constraints from the surrounding clean bits usually
still pin those bits down; the soft output keeps that as a graded `c_true`/`c_false`
lean, and a soft (or even argmax-projected) consumer can use it.

The hand-off itself is the same **erasure bridge** the
[concatenation example](concatenated.md) uses: feed the recovered codeword into
`rs251`'s encoded buffer and any non-boolean symbol becomes an RS erasure
automatically (see [rs251 → Erasures and invalid
symbols](bc/rs251.md#erasures-and-invalid-symbols)). With the soft decoder you have
two options:

1. **Soft codeword → soft RS.** Hand the `dt_soft_bit` stream straight to
   [`rs251`'s soft decoder](bc/rs251.md#soft-decoding). It assembles each symbol's
   hard value by per-bit argmax and, on a failed decode, iteratively erases the
   least-reliable still-"known" symbol — ranked by the largest `c_invalid +
   c_absent` over its bits — trading a cost-2 error for a cost-1 erasure until the
   block decodes.
2. **Argmax → hard RS.** Project each soft record to its hard symbol and use the
   ordinary hard `rs251` decoder. This already captures most of the benefit,
   because the win is in the soft decoder's value recovery, not in the soft RS's
   ranking.

### A runnable soft path

The [concatenation example](concatenated.md) is the runnable version: it drives the
`hybrid` soft decoder into the soft `rs251` decoder end to end, with the hard path
alongside for contrast. The only differences from a hard-decision pipeline are the
three named above — the decoder factory (`dt_cc_hybrid_soft_decoder_create`), the
output buffer type (`dt_soft_bit`), and the outer decoder
(`dt_bc_rs251_block_soft_decoder_create`). The drive contract is otherwise
identical: `begin` → `decode` (repeat) → `finalize`, with the same warm-up prefix,
drain-then-finalize loop, and flush-tail accounting (see the [streaming
interface](stream.md)).

### What this buys, and where it stops

That example shows the gain concretely: on `RS(40,24)` (budget 16) with 16-bit
erasure bursts, the hard path's inner decoder leaves residue `2·errors + erasures =
21`, exceeding the budget, and the block **fails**; the soft path on the identical
channel **recovers exactly**, because the soft decoder resolves the burst regions to
(correct) values the hard decision had erased. That gain is robust.

What it does **not** do is rescue confident misalignment. The soft RS's iterative
erasure pass only helps when `c_invalid + c_absent` actually flags the wrong
symbols — which holds for genuine deletions (`maxir`'s sharp `c_absent`) but not for
a phase slip past `max_drift`, where the wrong symbols carry low `c_absent` in every
decoder. On such channels the soft and hard paths fail together; the fix is more
inner redundancy or a larger `max_drift`, not a reliability flag. Treat the soft RS
ranking as a refinement on top of the soft decoder's value recovery, not as the
mechanism that carries the result.

## Choosing `hybrid` vs `maxir` for soft output

Both now emit the full alphabet, so the choice is about cost and about how much you
rely on the absent/invalid fields:

- Reach for **`maxir`** when you need the **faithful** soft output — in particular
  a true per-position deletion marginal (`c_absent` that pinpoints deletions
  independent of lock) or `DT_INVALID` round-tripping you intend to act on. It is
  the heavier decoder.
- Reach for **`hybrid`** when you want a lighter drift-tolerant soft decoder and
  the coarser `1 − c_locked` absent signal is acceptable — which it often is,
  because the dominant factor in whether an outer code recovers is the inner
  decoder's **value** accuracy, not the absent ranking.

That value-accuracy comparison between the two on a given channel is exactly what
the Monte-Carlo [metrics](../metrics) harness is for; prefer measuring it on
representative data over assuming a winner, since which decoder leaves less residue
is channel-dependent.

## See also

- [Symbols (`bit`)](bit.md) — the `dt_soft_bit` fields and predicates.
- [`bcjr`](cc/bcjr.md) / [`hybrid`](cc/hybrid.md) / [`maxir`](cc/maxir.md) — the
  three soft decoders' per-codec references.
- [`rs251` → Soft decoding](bc/rs251.md#soft-decoding) — the soft-input outer
  decoder and its iterative erasure pass.
- [Worked example: concatenation](concatenated.md) — the end-to-end pipeline this
  page's soft path extends.
- [Data-flow semantics](data_flow_semantics.md) — the symbol model the
  consistencies are graded over.
