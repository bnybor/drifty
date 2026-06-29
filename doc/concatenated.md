# drifty — worked example: concatenation

drifty's layers are meant to stack: a convolutional [`cc/`](cc/README.md) **inner**
code carries a continuous stream across a drifting channel and hands a clean,
fixed grid of [output-domain symbols](data_flow_semantics.md) to an **outer**
block code that cleans up what slipped through. This page wires the whole thing
together in one runnable program, so the inner→outer hand-off — the part the
per-layer pages only describe — is visible end to end.

The stack here is the standard one:

```
message ─▶ rs251 encode ─▶ cc encode ─▶ [channel] ─▶ cc decode ─▶ rs251 decode ─▶ message
            (outer)         (inner)                    (inner)      (outer)
```

The example runs the **soft** inner decoder, which is the better way to drive this
pipeline: the inner [`hybrid`](cc/hybrid.md) soft decoder absorbs the drift and the
bulk of the flips and reports, per transmitted bit, a graded
[consistency](soft_decoding.md) rather than a hard symbol — keeping the value
information that a hard decision discards over a damaged stretch. The outer
[`rs251`](bc/rs251.md) soft decoder consumes that stream, turns the positions the
inner code could not recover into Reed–Solomon erasures (the *erasure bridge*; see
[rs251 → Erasures and invalid
symbols](bc/rs251.md#erasures-and-invalid-symbols)), and corrects the rest. The
program also runs the **hard** path on the identical channel for contrast — on this
channel the hard path fails where the soft path recovers.

## The program

This is a complete program. It builds against the public headers and
`libdrifty.a` (see the [README → Build](../README.md#build)); compile it with
`cc concat.c libdrifty.a -lm`.

```c
#include <drifty/bc/rs251.h>
#include <drifty/bit.h>
#include <drifty/cc/ccode.h>
#include <drifty/cc/encoder.h>
#include <drifty/cc/hybrid.h>
#include <drifty/result.h>
#include <drifty/soft_bit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void byte_to_bits(unsigned char v, dt_bit *out) {
  for (int i = 0; i < 8; i++)
    out[i] = (v & (0x80u >> i)) ? DT_TRUE : DT_FALSE;
}
static int bits_to_byte(const dt_bit *in, unsigned char *out) {
  unsigned v = 0;
  for (int i = 0; i < 8; i++) {
    if (!DT_IS_BIT(in[i])) return 0; /* erasure/absent/invalid -> not a byte */
    v = (v << 1) | DT_BIT(in[i]);
  }
  *out = (unsigned char)v;
  return 1;
}
/* hard projection of a soft record (argmax over the alphabet) */
static dt_bit hard_of(dt_soft_bit b) {
  float mx = b.c_false; dt_bit a = DT_FALSE;
  if (b.c_true    > mx) { mx = b.c_true;    a = DT_TRUE;    }
  if (b.c_erasure > mx) { mx = b.c_erasure; a = DT_ERASURE; }
  if (b.c_invalid > mx) { mx = b.c_invalid; a = DT_INVALID; }
  if (b.c_absent  > mx) { mx = b.c_absent;  a = DT_ABSENT;  }
  return a;
}
/* count erased / silently-wrong symbols of a hard codeword vs truth */
static void residue(const dt_bit *rcw, const dt_bit *cw, int N, int *pe, int *perr) {
  int e = 0, err = 0;
  for (int s = 0; s < N; s++) {
    int fl = 0, wrong = 0;
    for (int i = 0; i < 8; i++) {
      if (!DT_IS_BIT(rcw[s * 8 + i])) fl = 1;
      else if (rcw[s * 8 + i] != cw[s * 8 + i]) wrong = 1;
    }
    if (fl) e++; else if (wrong) err++;
  }
  *pe = e; *perr = err;
}
static int msg_ok(const dt_bit *dec, const unsigned char *msg, int MSG) {
  for (int i = 0; i < MSG; i++) {
    unsigned char b;
    if (!bits_to_byte(dec + i * 8, &b) || b != msg[i]) return 0;
  }
  return 1;
}

int main(void) {
  const uint16_t N = 40, K = 24; /* RS(40,24): 16 parity symbols */
  const int MSG = K - 1;         /* 23 message bytes (GF(251) packing) */
  enum { DEPTH = 42, DRIFT = 4 };

  dt_cc_code *code = dt_cc_code_create_standard(DT_CC_CODE_K7_RATE_1_2);
  if (!code) return 1;
  const int ncc = dt_cc_code_n(code), kcc = dt_cc_code_k(code);

  unsigned char msg[64];
  for (int i = 0; i < MSG; i++) msg[i] = (unsigned char)(0x41 + i);

  /* ---- OUTER encode: rs251 -> the 8*N codeword bits are the inner payload ---- */
  dt_block_encoder *rse = dt_bc_rs251_block_encoder_create(N, K);
  dt_bit *rse_in = rse->decoded_buf(rse);
  for (int i = 0; i < MSG; i++) byte_to_bits(msg[i], rse_in + i * 8);
  rse->encode(rse);
  dt_bit *cw = rse->encoded_buf(rse);
  const int CWBITS = 8 * N;

  /* ---- INNER encode: prepend DEPTH known warm-up bits, then the codeword ---- */
  const int WARM = DEPTH, INFO = WARM + CWBITS;
  dt_bit *info = malloc((size_t)INFO);
  for (int i = 0; i < WARM; i++) info[i] = DT_FALSE;
  memcpy(info + WARM, cw, (size_t)CWBITS);
  const int CAP = (INFO + kcc) * ncc + 64;
  dt_bit *coded = malloc((size_t)CAP);
  dt_stream_encoder *cce = dt_cc_encoder_create(code);
  int clen = cce->begin(cce, coded, CAP);
  clen += cce->encode(cce, coded + clen, CAP - clen, info, INFO);
  clen += cce->finalize(cce, coded + clen, CAP - clen);
  dt_cc_encoder_destroy(cce);

  /* ---- CHANNEL: two 16-bit erasure bursts + one indel pair ---- */
  dt_bit *rx = malloc((size_t)clen + 8);
  int rlen = 0, di = clen / 5, ii = 4 * clen / 5, b0 = clen / 3, b1 = 2 * clen / 3;
  for (int i = 0; i < clen; i++) {
    if (i == di) continue;                          /* delete one coded bit */
    dt_bit b = coded[i];
    if ((i >= b0 && i < b0 + 16) || (i >= b1 && i < b1 + 16))
      b = DT_ERASURE;                               /* burst: evidence gone here */
    else if (i % 50 == 0 && DT_IS_BIT(b))           /* ~2% background flips */
      b = (b == DT_TRUE) ? DT_FALSE : DT_TRUE;
    rx[rlen++] = b;
    if (i == ii) rx[rlen++] = DT_TRUE;              /* insert one spurious bit */
  }

  dt_cc_hybrid_stream_params hp = {
      .decision_depth = DEPTH, .max_drift = DRIFT, .p_flip = 0.03f,
      .p_ins_true = 0.004f, .p_ins_false = 0.004f, .p_del = 0.008f,
      .p_ovr_erase = 0.02f };                       /* erasures can appear */
  const int OCAP = INFO + 64;

  /* ===== SOFT path: hybrid SOFT decoder -> soft rs251 ===== */
  dt_stream_soft_decoder *sd = dt_cc_hybrid_soft_decoder_create(code, &hp);
  dt_soft_bit *sout = malloc((size_t)OCAP * sizeof *sout);
  sd->begin(sd, NULL, 0);
  int sn = sd->decode(sd, sout, OCAP, rx, rlen);
  for (;;) { int g = sd->decode(sd, sout + sn, OCAP - sn, NULL, 0); if (g <= 0) break; sn += g; }
  sn += sd->finalize(sd, sout + sn, OCAP - sn);     /* drain + flush the tail */
  dt_cc_hybrid_soft_decoder_destroy(sd);

  dt_soft_bit *srcw = sout + WARM;                  /* skip the warm-up prefix */
  dt_bit sproj[4096];                               /* argmax, to gauge residue */
  for (int i = 0; i < CWBITS; i++) sproj[i] = hard_of(srcw[i]);
  int se, serr; residue(sproj, cw, N, &se, &serr);

  dt_block_soft_decoder *srs = dt_bc_rs251_block_soft_decoder_create(N, K, /*s=*/0);
  memcpy(srs->encoded_buf(srs), srcw, (size_t)CWBITS * sizeof(dt_soft_bit));
  int sok = (srs->decode(srs) == DT_OK) && msg_ok(srs->decoded_buf(srs), msg, MSG);

  printf("SOFT  (hybrid soft -> soft rs251):\n");
  printf("  inner argmax residue: %d erased + %d silently-wrong of %d symbols\n", se, serr, N);
  printf("  message recovered exactly: %s\n", sok ? "YES" : "NO");

  /* ===== HARD path on the SAME channel, for contrast ===== */
  dt_stream_decoder *hd = dt_cc_hybrid_decoder_create(code, &hp);
  dt_bit *hout = malloc((size_t)OCAP);
  hd->begin(hd, NULL, 0);
  int hn = hd->decode(hd, hout, OCAP, rx, rlen);
  for (;;) { int g = hd->decode(hd, hout + hn, OCAP - hn, NULL, 0); if (g <= 0) break; hn += g; }
  hn += hd->finalize(hd, hout + hn, OCAP - hn);
  dt_cc_hybrid_decoder_destroy(hd);

  dt_bit *hrcw = hout + WARM;
  int he, herr; residue(hrcw, cw, N, &he, &herr);
  dt_block_decoder *hrs = dt_bc_rs251_block_decoder_create(N, K, /*s=*/0);
  memcpy(hrs->encoded_buf(hrs), hrcw, (size_t)CWBITS);
  int hok = (hrs->decode(hrs) == DT_OK) && msg_ok(hrs->decoded_buf(hrs), msg, MSG);

  printf("HARD  (hybrid hard -> hard rs251), same channel:\n");
  printf("  inner residue: %d erased + %d silently-wrong; 2*err+eras = %d, RS budget n-k = %d\n",
         he, herr, 2 * herr + he, N - K);
  printf("  message recovered exactly: %s\n", hok ? "YES" : "NO");

  dt_bc_rs251_block_soft_decoder_destroy(srs);
  dt_bc_rs251_block_decoder_destroy(hrs);
  dt_bc_rs251_block_encoder_destroy(rse);
  dt_cc_code_destroy(code);
  free(info); free(coded); free(rx); free(sout); free(hout);
  return sok ? 0 : 3;
}
```

Running it prints:

```
SOFT  (hybrid soft -> soft rs251):
  inner argmax residue: 0 erased + 2 silently-wrong of 40 symbols
  message recovered exactly: YES
HARD  (hybrid hard -> hard rs251), same channel:
  inner residue: 11 erased + 5 silently-wrong; 2*err+eras = 21, RS budget n-k = 16
  message recovered exactly: NO
```

## Reading the output

**The soft decoder recovers what the hard decision discards — that is the whole
point.** On this channel the two erasure bursts drop the inner decoder's lock
below its hard-decision threshold, so the **hard** front end gives up on eleven
whole symbols (emitting `DT_ABSENT`/`DT_ERASURE`) and gets five more confidently
wrong: a load of `2·5 + 11 = 21` against an RS budget of `16`, and the block
fails. But those burst-region bits are not really lost — the convolutional
constraints from the surrounding clean coded bits still pin most of them down. The
hard cascade throws that information away; the **soft** front end keeps it as a
graded `c_true`/`c_false` lean, so its argmax projection leaves only `0` erased +
`2` silently-wrong — trivially inside budget — and the message comes back exact.
Same code, same channel, same outer parity; the only difference is reading the
soft values instead of a hard decision.

**The erasure bridge still does its job.** Whatever the soft decoder genuinely
cannot place arrives at `rs251` as a non-boolean symbol and becomes a
Reed–Solomon erasure automatically (see [rs251 → Erasures and invalid
symbols](bc/rs251.md#erasures-and-invalid-symbols)). What changed versus a hard
inner decoder is how *little* lands in that bucket, because the soft decoder
resolves to values where the hard one erased.

**Soft RS vs. argmax → hard RS.** The program feeds the `dt_soft_bit` codeword
straight into `rs251`'s [soft decoder](bc/rs251.md#soft-decoding), which assembles
each symbol by per-bit argmax and can iteratively erase its least-reliable symbols
(ranked by `c_invalid + c_absent`) on a failed decode. But note the `inner argmax
residue` line: the soft decoder's *hard projection* is already clean enough
(`0e+2`) that even projecting to hard symbols and using the ordinary hard `rs251`
decoder would recover here. The win is the soft **decoder**, not the soft RS's
ranking — the soft RS is a refinement on top, most useful when genuine deletions
carry sharp `c_absent` (see [Soft decoding](soft_decoding.md)).

**Output is one symbol per transmitted information bit — including the flush
tail.** Each decoder emits `WARM + CWBITS + (K − 1)` symbols: the `K − 1` extra are
the terminating bits the encoder's `finalize` appends to drain the shift register
(see [encoder → Driving](cc/encoder.md#driving-the-encoder)). They land after the
payload, so indexing the codeword as `out + WARM` is unaffected.

**Warm-up is a prefix you discard.** A bit is committed only after a
`decision_depth` look-ahead, and the decoder acquires lock blindly even at the
stream start, so the first `~decision_depth` output symbols are unreliable. The
program spends them deliberately — `DEPTH` known bits prepended, the matching
`DEPTH` output symbols dropped (`srcw = sout + WARM`). A real protocol would skip a
known preamble instead.

**The drain-then-finalize loop matters.** A `decode` that fills its buffer leaves
the rest buffered inside the decoder; the loop calls `decode` again with
`src_len == 0` until it stops making progress, then `finalize` flushes the
in-flight tail. With drift this is not optional — output trails input by the
decision depth plus whatever the alignment is doing — and the soft front end
behaves identically to the hard one here.

## When the hard path is enough

The hard inner + hard outer decoders are simpler and lighter, and on a milder
channel — or with enough outer parity to absorb the hard decoder's wider erasures —
they recover fine; at `RS(48, 24)` this same burst pattern decodes on the hard path
too. Reach for the soft path when you are bandwidth-limited on the outer code, when
the channel produces sustained low-lock stretches like the bursts above, or
whenever you want to extract the most from a fixed inner code. If you want the soft
decoder's value recovery without the soft outer decoder, project each `dt_soft_bit`
to its hard symbol (the `hard_of` helper above) and feed the ordinary hard `rs251`
decoder — that already captures most of the gain.

## Adding a frame layer

The example feeds the codec one block. To carry *many* variable-length blocks
back to back on the same stream, wrap the inner codec in a frame
[`fc/`](fc/README.md) codec: the [`marker`](fc/marker.md) encoder brackets each
payload with begin/end markers and keeps the stream transparent, and its decoder
re-discovers the boundaries — reporting them through `get_state()` — so the
receiver knows where each block starts and ends without a length field. The frame
codec carries no error correction of its own; it sits *outside* the `cc`/`bc`
pair, delimiting the protected payloads. Drive it through the
`begin → begin_frame → encode* → end_frame → finalize` phases on the encode side
and the `decode` + `get_state()` loop on the decode side exactly as
[its page](fc/marker.md#driving-the-decoder) shows; the per-frame payload between
the markers is then encoded and decoded by the inner/outer stack above.

## Tuning the margin

Two knobs set how much channel damage the stack absorbs:

- **Outer rate (`n − k`).** More parity tolerates more residue but costs
  bandwidth. The inner decoder's failures are *bursty* — when it loses lock it
  corrupts a run of consecutive symbols — so size `n − k` for the worst burst you
  expect, not the average symbol-error rate. The
  [`rs251` spare-symbol guard `s`](bc/rs251.md#spare-check-symbols-s) trades a
  little of that budget for protection against a confident-but-wrong decode.
- **Inner code, and hard vs. soft.** A stronger inner code (lower rate / higher
  free distance, e.g. `DT_CC_CODE_K5_RATE_1_5`) leaves less residue, at the cost of
  more coded bits per payload bit; the soft front end extracts more from whatever
  inner code you pick. The [metrics](../metrics) sweeps quantify the inner-code
  trade for the drift channels.

## See also

- [Soft decoding](soft_decoding.md) — the soft inner decoders, the full soft
  alphabet, and the `c_absent` differences between `hybrid` and `maxir`.
- [Convolutional coding (`cc/`)](cc/README.md) — the inner codecs and the
  [code](cc/ccode.md) they share.
- [`rs251` block codec](bc/rs251.md) — the outer code, the erasure bridge, and
  the soft-input decoder.
- [Data-flow semantics](data_flow_semantics.md) — why the inner→outer boundary
  carries only values and erasures on a fixed grid.
