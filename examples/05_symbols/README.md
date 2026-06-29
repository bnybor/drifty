# 05 — symbols

The non-boolean transmit symbols, round-tripping end to end. Input bits are usually
`DT_TRUE` / `DT_FALSE`, but drifty's alphabet has two more a sender can encode:

- **`DT_INVALID`** — a deliberate non-value ("poison"). The encoder marks the coded
  bits that carry it; the decoder recovers the slot as `DT_INVALID` (`c_invalid ~
  1`), never silently as a 0 or 1.
- **`DT_ERASURE`** — a value deferred to the channel. Told to expect erasures
  (`p_ovr_erase`), the decoder recovers the slot as `DT_ERASURE`.

## What it shows

- That the encoder carries `DT_INVALID` and `DT_ERASURE` inputs through to the
  coded stream, and the (soft) decoder recovers each as itself while the ordinary
  bits around them decode normally.

## Run

```sh
./build/examples/05_symbols/05_symbols
```

## Expected output

```
sent: msg[90] = DT_INVALID (poison), msg[150] = DT_ERASURE (deferred)

recovered:
  poison slot  90 -> I   (c_invalid=1.00)
  erase  slot 150 -> E   (c_erasure=1.00, c_invalid=0.00)
  ordinary bits around them decode normally: yes
```

## Reading it

- The poison slot comes back as `I` with `c_invalid = 1.00` — the decoder reports a
  deliberate non-value, not a guessed bit.
- The deferred slot comes back as `E` (`c_erasure = 1.00`, `c_invalid = 0`) — an
  unrecoverable value, distinct from poison. The model needs `p_ovr_erase > 0` to
  expect erased coded bits.
- Both differ from a *channel* erasure (04) in that the **sender** chose them; they
  round-trip by design (damage permitting).

## See also

- [Data-flow semantics](../../doc/data_flow_semantics.md) — the full `DT_` alphabet
  and the transmit vs output domains.
- [The encoder](../../doc/cc/encoder.md) — how it carries poison and deferral.
