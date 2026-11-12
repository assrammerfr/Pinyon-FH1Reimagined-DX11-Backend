# DX11 Finish — running state (update at EVERY step boundary)

## PHASE 17 — 2026-11-12: THE DRAW-DIFF RAN. THE HALF DRAW IS ISSUED AND WRITES BLACK; THE FAULT IS THE SAMPLE, NOT THE DRAW. BAND NOT FIXED.

TOOL ADDED (default-off, probe-only): the existing `fh1_debug_c2_probe` now also logs `after=` —
the per-60-row RGB nonzero bands of the surface *after* each draw it probes. The footprint says
which region a pass touched; `after` says what it left there, which separates "never drawn" from
"drawn and then blacked out".

WHAT THE DRAW-DIFF SHOWED (bad run `c2p2-run`, window 336, executor=1 readback=1 refresh=1):

| probe | draw | changed | after bands (rows 0-359 | 360-719) |
|---|---|---|---|
| #1 | world half 0, vp 0,0 1280x360, 3 verts, prim 8 | 0.114 | **0.15 x6** | 1.00 x6 |
| #2 | world half 1, vp 0,360 1280x360 | 0.499 | 0.15 x6 | 1.00 x6 |
| #3-#60 | frame-sized HUD/post draws | <= 0.0007 each | 0.15 x6 | 1.00 x6 |

So in a bad frame the top-half draw IS issued with the right rectangle, changes ~65% of its own
half, and leaves it at 15% nonzero: it paints its half **black**. No later pass takes the half away
- the 15% is already there at draw #1 and never moves. In a good run the same probe's `after` image
is the full scene (`.local/diag/c2p1/c2p-336-1-after.ppm`, viewed: snow, trees, a seam at y=360).

Corollary that kills the previous theory: the half is *not* inherited damage and *not* wiped later.
A black result from a draw that rasterized correctly means the pixel shader read black.

WHAT THE TWO HALVES SHARE (measured, `fh1_debug_texture_dump` at swap 336): both are
`vp 0,0 / 0,360`, both sample **the same** `t0 base=1C4E1000 guest=1280x720 fmt=54 tiled=1`, host
`1280x720 srv_fmt=24`. Same texture, same base, same format - so the difference between the halves
is only where in that texture each one samples.

The world texture's own construction is consistent, not the defect: three contiguous strips
(`+0x000000` 256 rows, `+0x140000` 256 rows, `+0x280000` 208 rows = 1C4E1000 / 1C621000 / 1C761000,
which is exactly 1280x(256+256+208)x4), all `0/32/4x/c12 src=0/32/4x/c3 by_owner=1`, 247+102
issues in a run. The layout lines up to the byte.

NEW EVIDENCE ON THE INPUT (`post-342-t0.ppm`, viewed): the host texture the composite samples is
sheared into diagonal stripes with a colour cast - the scene is recognisably there but rows drift
and the R channel reads 0 below ~row 300 while G/B stay non-zero. Two readings, not yet separated:
(a) the probe's own readback of a tiled host texture is unswizzled (probe artifact), or (b) the
untiling/upload of this texture is wrong, which would also explain a half that samples empty.
This is the open question, and it is why no fix is claimed.

RUNS ARE WHOLLY GOOD OR WHOLLY BAD (the sharpest new fact): pair8 (338 good), pair9 (338 good),
surf1 (338 good), texd1 (338 good, 342 bad *frame*), pair1/pair3/pair4 (every swap bad),
c2p2 (every swap bad). Same seed, same swap index, opposite outcomes - so the discriminator is
established once per run, not per swap.

GATES/REGRESSION on this binary (actual output):
- `PASS d3d11-draws (gate-draws-20261112b): draw_calls=124833, presented title frame; captures
  draws-frame [0-255]`, GATE_EXIT=0.
- `trace-short` on defaults: md5 **`b8f68e89c6192577747ab452aa5598ef`** on both captures.

NOT FIXED. The acceptance criterion (no band above row 360 across two seeds and repeated swaps) is
NOT met. SINGLE MOST INFORMATIVE NEXT EXPERIMENT: run `fh1_debug_guest_probe` in one known-bad and
one known-good run and compare, at both half draws, the guest bytes and the mirror bytes of t0 for
rows 0..719 against the untiled expectation - that separates "the strips never wrote those rows"
from "the untiles drops or shears them", and it lands on the exact buffer that the black half
samples.

## PHASE 16 — 2026-11-12: SAME-MOMENT PAIR SAYS THE UPLOAD LEG IS FAITHFUL AND THE COMPOSITE'S TOP HALF IS PRODUCED ONLY ON SOME SWAPS. NO FIX CLAIMED.

INSTRUMENT (default-off): `fh1_debug_presented_pair` + `_swap` + `_count` — ONE call in
IssueSwap, after RequestSwapTexture, dumps (a) the composite the presenting resolve sampled and
(b) the untiled swap texture the presenter paints, each with a per-60-row RGB nonzero/mean band
profile; both log lines carry the same `swap=` counter, which is the same-moment proof the old
surface dump could not give (it fired whenever its swap was reached, which was after the captures).
`_count` repeats it for N consecutive swaps. Also corrected the resolve source probe: it counted
whole words, so an 8_8_8_8 half of `0xC0000000` (RGB 0, alpha 192) read as 1.000 full; it now
masks RGB.

MEASURED (GTX 570, seed `.local/diag/p3m3-trace.state`, executor=1 readback=1 refresh=1):

| run | swap | composite (c2) bands nz | presented (swap texture) bands nz | verdict |
|---|---|---|---|---|
| pair1 | 340 | `0 0 0 0 0 0 .917 .927 .926 .841 .925 1.000` | identical, colours 1353 both | upload leg faithful on a bad frame |
| pair2 | 340 | all `1.000`, mean 69.5 -> 110.1 | identical (11000 vs 10999 colours) | faithful on a good frame too |
| pair3 | 345 | top `0.151` mean 18-25, bottom full | identical | faithful, partial top |
| pair4 | 335-344 (10 in a row) | top `0.000` on all ten, bottom full | identical | the band is steady over a window, not a one-off |

At the RESOLVE INSTANT (RGB-aware bands, same run, swaps 386-397): `1.000` on 387-389, `0.000`
on 386 and on 390-397. The top half is present before the copy exactly when it is present on
screen - so the resolve never reads a full frame that later loses a half, and the presenter never
loses what the resolve read.

WHAT THIS DECIDES, in the task's own terms:
- **Upload/untile leg: EXCLUDED.** On every one of 13 paired swaps the presented texture is
  band-for-band and colour-for-colour identical to the composite - including the bad frames. A leg
  that dropped the top half could not reproduce a missing half exactly.
- **"Composite draws landing after the presenting resolve": ALSO EXCLUDED as stated.** At the
  resolve instant the half is already missing in the bad swaps, so the draws did not arrive late;
  the surface simply never had the top half for that swap.
- **What is left:** the composite's top half is PRODUCED only on some swaps (bottom half every
  swap). Named, not fixed.

GATE (actual output, this binary): `PASS d3d11-draws (gate-draws-20261112): draw_calls=105959,
presented title frame; captures draws-frame [0-255]`, GATE_EXIT=0. Default path unchanged:
`trace-short` on defaults reproduces md5 **`b8f68e89c6192577747ab452aa5598ef`** on both captures.

NOT FIXED - the acceptance criterion (no black band, full 0..720 across seeds) is NOT met and no
fix is claimed. NEXT SINGLE EXPERIMENT: log the draws that write `0/16/1x/c2` (viewport rect,
predicate, MSAA key, and whether the executor claims their tiles) over a bad window (335-344) and a
good one (387-389) and diff them; and read `0/16/4x/c2`, which measured 0.002 nonzero, because if
the top half is meant to arrive through an MSAA resolve into the 1x surface, that resolve is the
thing to look at.

## PHASE 15 — RE-ENTRY 2026-11-12: BINARY REBUILT, THE DEPTH LEG FINALLY HAS AN INSTRUMENT, AND THE PRESENTED FRAME MEASURES 4.7% COVERAGE AGAINST A 34% COMPOSITE

RECON FROM DISK (not from any older summary):
- `out/build/win-amd64-release` held a CMakeCache dated 2026-11-12 01:06 and **no binary**; the last
  binary was parked in `out/build/win-amd64-release.bak-20261112` (2026-10-26 19:55).
  `.local\muse_build_game.bat` rebuilt the tree: **BUILD_EXIT=0**, 1225 steps, 0 `error:` lines.
- Default path unchanged after the rebuild: trace-short on defaults (seed
  `.local/diag/p3m3-trace.state`, `gpu_backend=d3d11`, `d3d11_adapter="GTX 570"`), both captures md5
  **`b8f68e89c6192577747ab452aa5598ef`** — the recorded baseline, marker for marker.
- Adapters present: GTX 570 (23.21.13.9135) + RX 5700 XT; the provider selects the 570 by name and
  creates the device at **feature level 11_0** (log line, every run below).

NEW INSTRUMENT — the depth leg had none. `fh1_debug_surface_dump` printed "depth surface (not
dumped)" for every d0/d1 surface, i.e. for exactly the surfaces the front-buffer draws sample.
Added `DumpDepthSurfacePpm` + `DepthDumpStats` (src/graphics/d3d11/command_processor.cpp, backup
`command_processor.cpp.musebak-20261112`): a same-format staging `CopyResource` of a depth surface,
word statistics, **both** D24S8 packings (low-24 and high-24 — the packing is implementation-
defined, so neither is assumed), a shaded PPM, and an explicit "multisampled depth (Nx) is not
readable" for MSAA depth (D3D11 staging cannot be multisampled and `ResolveSubresource` has no
depth support). Default-off; only `fh1_debug_surface_dump` reaches it.

MEASURED at the dump swap (run p15c, seed `.local/diag/p3m3-trace.state`, executor=1 readback=1
refresh=1, swap 340):

| depth surface | size | zero words | depth(lo) mean/min/max |
|---|---|---|---|
| 720/13/1x/d0 | 1040x2528 | 0.9083 | 0.0566 / 0 / **1.000** |
| 0/16/1x/d1 | 1280x2048 | 0.9757 | 0.0062 / 0 / 0.999 |
| 720/16/1x/d1 | 1280x2048 | **0.9987** | 0.0006 / 0 / 0.5 |
| 4/1/1x/d1 | 80x8192 | 0.8998 | 0.0379 / 0 / 0.999 |
| all 4x / 2x d0, d1 (720/16/4x, 0/16/4x, 1024/16/4x, 128/4/4x, 0/16/2x, 720/13/4x, ...) | - | n/a | `multisampled depth (Nx) is not readable` |

So the 1x depth surfaces do hold geometry depth (720/13 reaches 1.0). **The scene depth the post
passes sample is the 4x MSAA family, and that is still unmeasured** — the next instrument gap.

PRESENTATION, quantified at the same swap:
- **Source** `0/16/1x/c2`: nonzero 0.3409 (= the 720 used rows of 2048, 3303 colours); every 60-row
  band over rows 0-719 is 1.000 nonzero, mean 84-147. The world is there (R/B-swapped, red palate).
- **Presented** (same run, `roam-3600.ppm`): rows 0-359 nz 0.13, mean 14-21; rows 360-719 nz
  0.83-1.00, mean 149-194 — a black band above row 360. The other captures repeat the shape
  (means 109.7 / 48.7 / 58.3 / 64.5, all game_mode 17); p15a gave 97.9 / 110.1 / 54.2 / 66.9.
- **The resolve that presents it** (call 17720): key `0/16/1x/c2`, rect `0,0-1280,720`, dest
  `1C149000` pitch 1280, `info=000E0041` (pack=1, endian=0), 3,686,400 B, and the pre-dispatch source
  probe reads **in_rect=8240/8240 nonzero, first=FF7F7F00**.
- **The fetch** is `fmt=54` = `xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16` (xenos.h), which is
  exactly the resolve's pack 1. The PHASE 12b format hypothesis — the resolve storing one pack while
  the fetch reads another — is **eliminated: they agree**.
- **The readback lands**: `frontbuffer readback ... pages_valid=920 gpu_written=920`, and 28,536 of
  29,440 sampled words (97%) are nonzero in guest memory after the copy. Guest memory is not empty
  in the presented extent, and the capture is not the frozen notice.

CONSEQUENCE: the loss is not the store, not the destination pack, not the mirror readback. What is
left is (a) the cache's **untiled upload of `k_2_10_10_10_AS_16_16_16_16`** for the swap texture
(host format `DXGI_FORMAT_R10G10B10A2_UNORM`, ConvertRunCopy, texture_cache.cpp) and (b) the **order
of the composite's two half-frame draws relative to the front-buffer resolve** — a half-frame is
exactly the shape of the missing region, and the run-to-run spread above is what an ordering race
looks like.

CAVEAT, recorded rather than papered over: the surface dump fires at
`presented_output_frames_ >= fh1_debug_surface_dump_swap`, and in p15b/p15c the dump landed after
the captures were taken, so the c2/frontbuffer comparison is same-swap at the dump but **not proven
same-moment as the capture**. Closing that means capturing at the dump swap.

NEXT TEST (named, in order): 1) capture at the dump swap so source and presented frame are the same
instant; 2) count nonzero words per 60-row band of the untile scratch buffer at the swap upload
(`load_scratch_base_`, texture_cache.cpp) — if the untile is faithful the band profile matches c2's,
which moves the question to ordering; 3) log the two c2 half-frame draws and the front-buffer
resolve with their swap index in one stream (the composite dump path already has the hooks).

NOT EVIDENCE OF REGRESSION: a run on seed `.local/diag/p3m3-roam2-off.state` **without** the
executor/readback env presents 33.453 on all four captures — that is the frozen notice, which is
the documented behaviour with `fh1_d3d11_resolve_readback` off, not a new defect.

## PHASE 13B — BRANCH B AS DECLARED: THE PROBE HASHES ARE REAL, THE ONE-WRITER INVARIANT IS NOW MEASURED IN A LIVE WORLD SWAP, AND t5 IS STILL THE c0 WORD FIELD (2026-10-25)

JOB: the FILLED Phase-13 prompt declares Branch B from the 12b probe and asks for the
separation fix, a one-writer census at 1CA95000, and a t5 dump that is a colour image.
ANSWER: the quoted hashes are verified from the log, bit for bit. The separation is
implemented and now measured in a live world swap (guard gated to start at swap 315; 92
skipped writers): 1CA95000 has **exactly one writer surface**, the 640x360 8_8_8_8 c0
resolve. t5 is still that c0 image after R/B restore (**0 / 230,400** texels differ) and is
still a two-channel word field - the word share **rises** (58.60% -> 72.49%) and the
distinct triples **fall** (17,889 -> 13,245). The guarded frame also stops being a moving
world (captures 213.6-222.3 mean, 3600 vs 4800 MAE **7.967**, gate minimum 10.000). No fix is
claimed: the acceptance numbers do not move toward a colour frame, they move away.

### STEP 1 — the declaration's hashes, verified (not re-derived)
`p13.log.state/logs/runtime.log`, same 12b probe, swap 318:
```
call=7845 range=1CA95000+2166784 ... before=E47845A573939074/33411 after=3D7BE7B375CA2518/9196 ok=true changed=true   (line 4287)
call=7926 range=1CA95000+983040  ... before=81B3078CE6B2BA6B/32159 after=192C643704E58C65/31505 ok=true changed=true    (line 4389)
call=7927 same extent            ... before=192C643704E58C65/31505 after=192C643704E58C65/31505 ok=true changed=false   (line 4391)
```
The 16 probes / 8 pairs are exact: calls 7645/7646, 7740/7741, 7833/7834, 7926/7927,
8019/8020, 8114/8115, 8210/8211, 8307/8308 - `true` then `false` in every pair. The `false`
repeat is the same resolve issued twice into the same extent (same source, same destination),
so it cannot show "no later writer"; it only shows the repeat is a no-op. Branch A is
refuted and Branch B's first clause (the store landed) holds.

### STEP 1, second clause — refuted by the published bytes, again
From the 12b dumps, recomputed here: `tex12b/world-320-t5.ppm` (230,400 texels) R/B-restored
equals `probe12b/0_16_1x_c0-320.ppm` cropped to 640x360 with **0 / 230,400** differences.
Exact RGB 0x7F,0x7F,0x00 = 135,006 = **58.5964%**; B==0 **84.9848%**; distinct triples
**17,889**; mean **(0.531049, 0.523453, 0.119707)**. The sampled bytes are the c0 surface's
own output, not surviving c3/depth bytes, so "something rewrote the base after c0" has no
support in the bytes the upload published.

### STEP 2 — the separation, now with a one-writer world swap
Added `fh1_debug_resolve_alias_from_swap` (default 0 = no gate; defined in flags.cpp,
declared locally in command_processor.cpp like the mode flag). With a base set, the guard
engages only from that presented-frame index, so the title can boot on the unguarded path
(the global guard stalled it - p15) and a live world swap can be observed with the base cut
to one writer. Run **p20 `20261026T015930Z-p24500`** (seed `p13.log.state`, executor=1,
shader pack, readback=1, refresh=1, swap_fetch_trace=1, store_probe_from=315,
resolve_trace=315, composite_draws=315, alias base 1CA95000, mode 0, from_swap 315):
**92 skips** = 36 depth (720/13/1x/d0) + 56 c3 (28 x pack 1, 28 x pack 0). Census at world
swap 320:

| call | writer | rect | pitch | pack | bytes | extent | guard |
|------|--------|------|-------|------|-------|--------|-------|
| 7882 | 720/13/1x/d0 | 520x520 | 1024 | 4 | 1,081,600 | 1CA95000+2,166,784 | skipped |
| 7946 | 0/16/1x/c3 | 320x192 | 320 | 1 | 245,760 | 1CA95000+245,760 | skipped |
| 7963 | 0/16/1x/c3 | 320x192 | 320 | 0 | 245,760 | 1CA95000+245,760 | skipped |
| 7966 | 0/16/1x/c0 | 640x360 | 640 | 0 | 921,600 | 1CA95000+983,040 | written |
| 7967 | 0/16/1x/c0 | 640x360 | 640 | 0 | 921,600 | 1CA95000+983,040 | written (repeat, no-op) |

Swap 318 has the same shape (7689 d0 / 7753 c3 pack 1 / 7770 c3 pack 0 skipped; 7773/7774
c0 written). The Branch B invariant - **exactly one writer surface at 1CA95000** - is now a
measurement, not an argument. (The two c3 copies per swap put this run's swap 320 where
p12b first saw the pack-0 copy, swap 323.)

### STEP 3 — the Phase-12 measurements under the one-writer guard
| swap | word 0x00007F7F | B==0 | distinct triples | mean RGB | R/B restore vs c0 |
|------|-----------------|------|------------------|----------|-------------------|
| 318 | 73.3702% | 82.2982% | 13,717 | (0.516296, 0.522656, 0.127006) | 0 / 230,400 |
| 319 | 76.0582% | 83.6845% | 9,679 | (0.507042, 0.514652, 0.100212) | 0 / 230,400 |
| 320 | **72.4857%** | **83.1753%** | **13,245** | **(0.518597, 0.520172, 0.119281)** | **0 / 230,400** |

Unguarded swap 320 for comparison: 58.5964% / 84.9848% / 17,889 / (0.531049, 0.523453,
0.119707) / 0 diffs. The guard makes the word field **worse**: the single word's share rises
by ~14 points and the triple count falls by ~4,600. Guest probe at swap 318:
`t5 base=1CA95000 ... guest_nz=230400/230400 distinct=503 mirror=1 same=1 first=7F7F0000` -
the upload is still faithful to the last writer, and the last writer is c0.

### The frame does not survive the invariant
p20's captures (`p20.log/roam-*.ppm`) are 213.563 / 220.442 / 219.348 / 222.270 mean; 3600
vs 4800 MAE **7.967** (the scenario gate needs 10.000): a bright, near-static frame, not the
world. The same seed unguarded (p13) gives 69.457 / 118.248 / 104.019 / 103.560. So reducing
1CA95000 to one writer costs the presented frame: the c3/depth writes are load-bearing
downstream even though they are dead for the t5 sample. This is also the answer to "give the
downscale its own address, or redirect": a redirect puts the same bytes at another base,
which is exactly what the skip does for every consumer of 1CA95000. The skip is the stronger
test, and it breaks the frame. p20 is therefore a trace run, not a gate pass (no result.json;
the capture-difference gate failed).

### Recorded, not changed
The swizzle asymmetry stands: t5's fetch is `0x60A` (srv_fmt 28), the c0 resolve's info word
is `000E0050` with `copy_dest_swap=1` applied, and the fetch swizzle is dropped, so even
correct c0 rows arrive R/B-swapped (0x7F7F00 vs 0x007F7F). The guard does not touch it; it is
a change to every fetch's channel order and needs its own acceptance frame.

### Verified
- Build `.local\\muse_build_game.bat`: **BUILD_EXIT=0** (16 steps, final binary with the gate).
- Default path unchanged: trace-short on defaults, seed carrying `gpu_backend="d3d11"` and
  `d3d11_adapter="GTX 570"`: **RUN_EXIT=0**, both captures md5
  **`b8f68e89c6192577747ab452aa5598ef`** (`.local/diag/trace16.log`).
- No default flips: `fh1_native_executor` 0, `fh1_d3d11_resolve_readback` false,
  `fh1_resolve_guest_refresh` off; the alias base is empty and `from_swap` 0 by default.
- Environment caution: a fresh state root has no `gpu_backend`, defaults to Vulkan and picks
  the **RX 5700 XT** (observed in the first trace15 attempt). Probes must use a seed carrying
  `gpu_backend = "d3d11"` and `d3d11_adapter = "GTX 570"`; `trace14.log.state` and
  `p13.log.state` both do.

## PHASE 14 — THE 1CE2D000 COPY READS A SURFACE THE FRAME NEVER FILLS; THE BY-OWNER LOOKUP RETURNS THE COPY'S OWN KEY, NOT AN ALTERNATE SURFACE (2026-10-24)

JOB: trace the 1280x720 front-buffer resolve at 1CE2D000 that writes ~98% zeros, from its source
surface's tiles to the dispatch's source-by-owner fallback. ANSWER: the resolve reads EDRAM key
`720/16/1x/c0`; all 720 tiles under the copy rectangle are owned by that same key, so
ResolveSourceOwner returns the copy's own key, the by-owner branch is not taken (322/322 resolves
`by_owner=0`), and the dispatch falls back to `GetOrCreateSurface(plan.resolve_key)` - the surface
the frame's own front-buffer draws already created. That surface is ~99% zero at the resolve
(median 327 of 23,440 sampled texels), and the copy is faithful to it. 1CE2D000 is never the swap's front buffer (0/373 swap traces), so this
copy is not the presented frame's black band; PHASE 13's attribution of the band to it is refuted.

### The chain, with the numbers
Run **p19 `20261024T195437Z`** (373 swaps, 322 front-buffer resolves), swap 370, calls 17705/17706:
```
resolve trace: call=17705 key=720/16/1x/c0 rect=0,0-1280,720 dest_base=1CE2D000 dest_pitch=1280
  info=000E0050 pack=0 endian=2 bytes=3686400 extent=1CE2D000+3768320 src=720/16/1x/c0 by_owner=0
census: tiles=720 unowned=0 owners= 720/16/1x/c0:720 mixed=0 class=0 result=720/16/1x/c0
source probe: 1280x2048 nonzero[sampled]=89 of 23,440 first=00000000
staging: 748/942,080 dwords nonzero (0.079%) first=00000000
call=17706 (after the 4-vert draw): probe 397, staging 2,679/942,080 (0.284%) first=0000FF00
```
- **Tiles.** Base tile 720, pitch 16, 45 tile rows x 16 columns = 720 tiles (EDRAM 720..1439 of
  2048). The census at every front-buffer resolve reads the same key on all 720, no unowned, no
  mixed owner, no MSAA/depth class mismatch.
- **Dispatch.** `src/graphics/d3d11/command_processor.cpp:2632-2640`:
  `source_by_owner` is only set when `source_owner != kNoOwner && source_owner != plan.resolve_key.Pack()`.
  Here the owner *is* the copy's key, so `source_by_owner` stays null and the source is
  `GetOrCreateSurface(plan.resolve_key)`. The key log's `surfaces 32->32` (p18, first world resolve)
  and 322/322 `surfaces N->N` in p19 show the surface already existed: the frame's front-buffer
  draws created it, the resolve did not create a blank one. The fallback is correct; the surface it
  returns is the one the draws claimed.
- **Content.** The surface is blank at the resolve. p18 swap 379: source probe 0 of 23,440, staging
  64 dwords of 942,080 (the store-probe marker). p19 over 322 resolves: probe min 0 / median 327
  (1.4% of the sampled texels) / max 8,228 (35%). The resolve copies exactly that.

### The only writers of the surface, and the loop that keeps it blank
p19 front-buffer draw census: **12 draws per swap**, every one binding `720/16/1x/c0` as its only
colour target (slot 1, mask 0xF, viewport and scissor 0,0 1280x720):

| draws | verts | samples |
|---|---|---|
| #1-#11 | 24/36 | `t0=1DAC5000:1280x720:f23:t1` (the resolved depth) + `t1=1D295000/1CE95000/1CA95000/1D6C5000:1024x1024:f22:t1` |
| #12 | 4/6 | `t1=1CE2D000:1280x720:f6:t1:e2:s60A` (its own guest-memory copy) |

Order at swap 370: draws #1-#11 (13:56:16.347-.382) -> resolve 17705 (.387) writes 1CE2D000 ->
**draw #12 (.400) samples 1CE2D000 and writes the 720/16 surface back** -> resolve 17706 (.405) copies
again (first texel `0000FF00`). That is a closed loop: the 12th draw's only input is the resolve's
own output, and the 11 depth-sampling draws leave the surface at ~0.1-0.4% of the frame (source
probe 89/23,440; first staging 748/942,080 including the probe marker). The world never enters this
buffer. (The depth those 11 draws read, `1DAC5000`, is itself only 2,390/942,080 dwords nonzero -
0.25% - in the p18 depth-resolve staging; the next hop from here is that depth copy, not this
resolve.)

### Where the world actually goes
- The composite surface is `0/16/1x/c2`: p18 surface dump at swap 320 gives nonzero 0.343, 994
  colours, mean 0.241,0.010,0.010; its resolve (info `000E0041`/`000E0051`) writes 1C149000 or
  1BDB1000.
- **1CE2D000 is never the presented buffer**: 0/373 swap traces have `fb_ptr=1CE2D000` (187 are
  1BDB1000, 186 are 1C149000; the last resolve for every swap is `000E0041` x311 / `000E0051` x61,
  never `000E0050`), and 0/373 frontbuffer readbacks read that range.
- So the 98% zeros is real but off-screen: it is not the presented frame's black band. The content
  "stops being the world" at the 720/16 surface itself - the frame's front-buffer draws never put
  the world there - not in the resolve, the mirror store, or the texture upload.

### Instrumentation and gates
- `fh1_debug_frontbuffer_draws` now runs outside the `fh1_debug_composite_draws` gate and names the
  front-buffer family in **any** colour slot; it was color0-only and nested inside the composite
  gate, which is why it logged nothing in p18. Default 0; no behaviour change.
- p18 `20261024T194251Z`: 477 swaps / 202 front-buffer resolves; simulation-time ratio 0.945892
  (< 0.95) - gate failed on probe overhead, trace data valid.
- p19 `20261024T195437Z`: 373 swaps / 322 resolves; distinct presentation cadence 4.083 Hz (< 5.0)
  - gate failed on probe overhead, world rendered (capture means 112.3 / 111.6 / 53.4 / 59.5).
- Build `.local\muse_build_game.bat`: **BUILD_EXIT=0**; all diagnostics default-off, executor run by
  environment only.
- Default path unchanged: trace-short on defaults (**no env cvars**) RUN_EXIT=0, both captures md5
  **`b8f68e89c6192577747ab452aa5598ef`** (`.local/diag/trace14.log/t0300.ppm`, `t0600.ppm`).

## PHASE 13 — 1CA95000 IS NOT POISONED: THE SAMPLE IS THE c0 PASS'S OWN DEPTH CONVERSION, AND THE BASE'S OTHER WRITERS CANNOT BE SEPARATED (2026-10-24)

JOB: lower the store-probe gate to 512 KiB, re-run one world swap, declare Branch A or B from the
mirror hashes, implement that branch's fix, and prove the t5 dump is a c0 colour image. ANSWER: the
gate was already lowered in 12b and the probe is **conclusive against the premise**. The c0 store
lands and is the last write at 1CA95000; the sampled bytes are the c0 resolve's own output (a
depth-derived RG mask produced by the c0 pass), not surviving c3/depth bytes. The separation fix was
implemented behind default-off cvars and measured: skipping the base's c3 writer stalls the title
before the world, and skipping the depth writer leaves t5 exactly the c0 image. **No fix is claimed**
- the acceptance criterion (a colour frame at t5) is not reachable from this base.

### STEP 1 — branch declaration, with the hashes
Probe run **p13 `20261024T072306Z-p13140`** (GTX 570, hidden, executor=1 + readback=1 +
resolve_guest_refresh=1 + swap_fetch_trace=1 + store_probe_from=315), swap 318 = calls 7836..7927:

```
call=7926 range=1CA95000+983040 rect=0,0x640,360 dest_base=1CA95000 pitch=640
  info=000E0050 (pack=0 endian=2 swap=1 ...) surface=0/16/1x/c0
  mirror before=81B3078CE6B2BA6B/32159 after=192C643704E58C65/31505 ok=true changed=true
call=7927 same extent ...
  mirror before=192C643704E58C65/31505 after=192C643704E58C65/31505 ok=true changed=false
```

The depth copy in the same swap (call=7845, 2,166,784 B) also lands:
`before=E47845A573939074/33411 after=3D7BE7B375CA2518/9196 changed=true`. So **Branch A is false**:
the mirror's after-hash does reflect the c0 resolve, and the swap's repeated copy is a no-op, not a
failed store. The c3 320x192 copy (245,760 B) is below the probe's gate and is not hashed, but the
c0 copies follow it by ~40 ms in the same swap and cover the whole 921,600-byte texture.

Branch B's first clause holds (the store landed). Its second clause does **not**: the sampled
texture is not carrying the c3/depth writes. Measured from the p12b swap-320 dumps:

- `world-320-t5.ppm` (230,400 texels) after R/B restore is **byte-identical** to
  `probe12b/0_16_1x_c0-320.ppm` cropped to 640x360: **0 / 230,400 texels differ**. The t5 sample is
  the c0 surface itself, one channel flipped - not a mixture with the c3 copy.
- The draw-time guest probe agrees: at `swap=320 t5 base=1CA95000` it reports
  `guest_nz=230400/230400`, `distinct=646`, `mirror=1`, `same=1`, `first=7F7F0000` - guest memory
  and the mirror are the same bytes, so the upload is faithful to the last writer.
- Why the c0 image looks like a depth field: in the same swap draw#2 (640x360, 3 verts) samples
  `t2=1DAC5000:1280x720:f6:t1`, and 1DAC5000 is the destination of the **pack-4 depth resolves** at
  calls 12038 (3,686,400 B) and 12095 (4x d1, 1,310,720 B). draw#2 writes c0; call 12118 resolves c0
  to 1CA95000; draw#3 then samples `t3=1CA95000:640x360:f6:t1`, writes c0 again; call 12119 resolves
  it; draw#4 samples `t5=1CA95000:640x360:f6:t1`. The t5 texture is a depth conversion made by the
  guest's own two passes - its 0x7F/0x7F/0x00 field is the encoding, not corruption.

So neither branch's causal claim describes the content: A's ordering is already right, and B's
"c3/depth bytes survive" is refuted by the byte-for-byte c0 match. The probe is not inconclusive;
the premise it was written to decide is.

### STEP 2 — the separation fix, implemented and measured (default off)
Added `fh1_debug_resolve_alias_base` (string, default empty), `fh1_debug_resolve_alias_bytes`
(default 921,600) and `fh1_debug_resolve_alias_mode` (0 all base writers, 1 depth only, 2 colour
only) in flags.{h,cpp}; the D3D11 IssueCopy skips and logs any resolve whose **destination starts at
the protected base** but is not the 640x360 8_8_8_8 writer (`FH1 resolve alias guard: skipped
call=... key=... rect=... dest=... extent=...`). The resolve trace still records every writer, so the
census survives the skip. Three measurements, each with the same seed and env as p12b/p13:

| run | guard | skips | result |
|-----|-------|-------|--------|
| p14 `20261024T182646Z-p2440` | whole extent | 14: the c3 320x192 base copy **and** its mip chain (1CAD1000/1CAE0000/...) | never leaves the boot/legal screen; captures 3600/4800 MAE **0.000** |
| p15 `20261024T184647Z-p17928` | base only, mode 0 | **177**, all c3 320x192 pack 1 at 1CA95000 | stalls before the world (t0 mean 0.0001, t2 zero); captures MAE **0.000** |
| p16 `20261024T185036Z-p17496` | base only, mode 1 (depth) | **15**, all 520x520 d0 pack 4 at 1CA95000 | reaches a world draw at swap 315 (t0 mean 0.0752, 2,595 colours); route lags (tutorial at 3600, world view by 5400) and the cadence gate fails: 3.813 Hz < 5.0 |

The 320x192 c3 resolve at 1CA95000 is **live**: the title's pre-world frames need it, so the base
cannot be reduced to one writer - skipping it stops the run before free roam. The wide-range version
also shows why "protect the whole 921,600-byte extent" is wrong: the 320x192 surface's mip chain is
allocated *inside* that extent (1CAD1000, 1CAE0000, 1CAFE000, 1CB04000, ...), and those resolves are
unrelated live surfaces.

With the depth writer skipped (p16), at swap 315 the world draw's t5 is still the c0 image:
`nonzero=0.9975 mean=0.4963,0.4945,0.0096 colours=116` (log sampling; 1,607 distinct RGB triples
over all 230,400 texels), 97.75% of texels exactly RGB (0x7F,0x7F,0x00), B==0 in 98.7%, and
R/B(t5) == the c0 surface dump again (**0 / 230,400 differ**). Guest probe at that draw:
`t5 ... guest_nz=230400/230400 distinct=63 mirror=1 same=1 first=7F7F0000`.

### STEP 3 — the Phase-12 measurements, recomputed from the same swap-320 dumps
| measure | p12b swap 320 t5 | c0 surface (same swap) |
|---|---|---|
| texels exactly RGB (0x7F,0x7F,0x00) / little-endian 0x00007F7F | **58.6%** | 58.6% as (0x00,0x7F,0x7F) |
| B == 0 | **84.98%** | 0.28% |
| distinct RGB triples | **17,889** | **17,889** |
| mean RGB | **(0.5310, 0.5235, 0.1197)** | (0.1197, 0.5235, 0.5310) |
| R/B restore vs c0 | **0 diffs** | - |

The dump is a faithful c0 image **and** it is a two-channel word field: those are the same fact.
The acceptance "must collapse, must be a colour render" cannot be met by removing writers, because
no writer other than c0 contributes to the sample.

### Writer census, swap 320 (dest_base 1CA95000)
| call | writer | rect | pitch | pack | rect bytes | extent |
|------|--------|------|-------|------|-----------|--------|
| 12037 | 720/13/1x/**d0** | 520x520 | 1024 | 4 (depth) | 1,081,600 | 1CA95000+2,166,784 |
| 12101 | 0/16/1x/**c3** | 320x192 | 320 | 1 (2_10_10_10) | 245,760 | 1CA95000+245,760 |
| 12118 | 0/16/1x/**c0** | 640x360 | 640 | 0 (8_8_8_8) | 921,600 | 1CA95000+983,040 |
| 12119 | 0/16/1x/**c0** | 640x360 | 640 | 0 (8_8_8_8) | 921,600 | 1CA95000+983,040 |

Inside the 921,600-byte window but at other destination bases: the c3 mip chain - 1CAE0000
(160x192, 122,880 B), 1CAD1000 (160x96, 61,440), 1CB04000 (96x96, 36,864), 1CAFE000 (96x64, 24,576),
1CB0F000 (64x64, 16,384), 1CB0D000 (64x32, 8,192), 1CB13000/1CB14000 (32x32, 4,096). From swap
323 on, some swaps also issue a **c3 pack-0** copy at the base (call 12396, 245,760 B, pitch 320)
before the c0 copies. Every one of these is dead for t5 (c0 follows), and the c3 base/mip copies are
live for other consumers.

### The swizzle asymmetry (recorded, not changed)
t5's fetch is swizzle `0x60A`, srv_fmt 28 (R8G8B8A8_UNORM_SRGB); the c0 resolve's info word is
`000E0050` - `copy_dest_swap=1` is applied, while the backend drops the fetch swizzle. That is the
whole of the t5-vs-c0 R/B difference (`0x7F7F00` vs `0x007F7F`). It is left in place: flipping it is
a change to every fetch's channel order, not a base-separation change, and it needs its own acceptance
frame. The content question is settled without it.

### Verified
- BUILD_EXIT=0 (`.local\muse_build_game.bat`, final binary with the guard and its three cvars).
- Default path unchanged: trace-short on defaults (**no env cvars**) RUN_EXIT=0, both captures md5
  **`b8f68e89c6192577747ab452aa5598ef`** on the final binary
  (`.local/diag/trace13b.log/t0300.ppm`, `t0600.ppm`).
- No default flips: `fh1_native_executor` stays 0, `fh1_d3d11_resolve_readback` stays false,
  `fh1_resolve_guest_refresh` stays off; the guard is empty by default.

### Still broken, and now the next lead
The presented free-roam frame's black band is **not** this base. The p13 probe localizes it to the
front buffer's own resolve: call=7847 `range=1CE2D000+3768320` (`720/16/1x/c0`, pitch 1280, swap=1)
goes from **11,311 nonzero stride-16 samples to 4,413** of 235,520
(`before=635BC3C7CE5DF9F6 after=127859752A3E7D39`), i.e. that dispatch writes ~98% zeros over the
1280x720 front buffer (the repeat at 7848 is a no-op). That is the frame the swap presents, and it
is downstream of c2 and of 1CA95000.

## PHASE 12 — 1CA95000 IS FOUR WRITERS AND ONE FETCH: THE WORLD IS THERE, IN THE WRONG CHANNEL ORDER (2026-10-24; corrected by 12b)

TRACE, not a fix. Session p12b `20261024T063625Z-p30964` (GTX 570, executor=1 +
resolve_guest_refresh=1), swap 320, the world composite frame. The 640x360 `f6` texture at
**1CA95000** (`srv_fmt=28`, fetch swizzle 0x60A, tiled, 921,600 bytes) is written **four times in
one swap** by three different surfaces, and only one of those writers is a 640x360 `8_8_8_8`
colour image:

| call  | writer                          | rect    | pitch | dest format (pack)     | bytes     |
|-------|---------------------------------|---------|-------|------------------------|-----------|
| 12037 | resolve 720/13/1x/**d0** (depth)| 520x520 | 1024  | depth (pack 4)         | 1,081,600 |
| 12101 | resolve 0/16/1x/**c3**          | 320x192 | 320   | **2_10_10_10** (pack 1), exp bias -2 | 245,760 |
| 12118 | resolve 0/16/1x/**c0**          | 640x360 | 640   | 8_8_8_8 (pack 0), swap 1 | 921,600 |
| 12119 | resolve 0/16/1x/**c0**          | 640x360 | 640   | 8_8_8_8 (pack 0), swap 1 | 921,600 |

The fetch that reads it: composite **draw#3** (`t3`) and **draw#4/#5** (`t5`), base 1CA95000,
640x360, `fmt=6` (`TextureFormat::k_8_8_8_8`), 4 B/texel, 2,560 B/row. The c3 copy writes
320x192 x 4 B = 245,760 B at **1,280 B/row**, so each of its rows occupies *half* a destination row:
destination row k = [c3 row 2k | c3 row 2k+1], 96 rows in total, every texel a packed 10:10:10:2
word decoded as four 8-bit channels. The depth copy alone (1,081,600 B) is **larger than the whole
921,600-byte texture**.

### What the bytes say (measured, not inferred)
`tex12b/world-320-t5.ppm` (the dump of the bound texture) has 230,400 texels, of which **60% are
the single word `0x00007F7F`** in *every* 16-row band (57-61%, no boundary at row 96 or 192), only
**17,889 distinct RGB triples**, `B==0` in 85% of texels, mean (0.531, 0.524, 0.120). R/B-restoring
it reproduces the c0 surface dump byte-for-byte, so the cache's upload is **faithful** to the
mirror - nothing is lost in the untile. `0x00007F7F` is a 10:10:10:2 word of fields
(0x37F, 0x1F, 0, 0) ~ (0.87, 0.03, 0, 0): a two-channel image's bytes, not a colour render.

### Where the content stops being the world (as first read - see 12b, which refutes it)
First reading: at **resolve call 12101** (c3 -> 1CA95000) the 320x192 2_10_10_10 world-downscale is
written at pitch 320 into the address a 640x360 `k_8_8_8_8` fetch and the two c0 resolves claim,
and the untiling upload reads the fetch's 921,600-byte extent - four times that write. That was
wrong: the probe in 12b shows the c0 copies overwrite the whole extent afterwards, every swap.
What is real and still live: the c0 resolve's `copy_dest_swap=1` *is* applied while the fetch's
swizzle 0x60A (R<->B) is dropped, so the world arrives R/B-swapped (`p12b-t5-rb.png`), and the
`c3` copy is a 245,760-byte window of the same range for whichever consumer samples before the c0
copies.

Added (log only, behind `fh1_debug_composite_draws`): the composite source dump now prints each
source's fetch swizzle and endianness (`:e2:s60A`), so the two swizzle facts can be read from one line. No fix is claimed and
the default path is untouched.

Was open: whether the c0 (world) resolve's write reached the mirror before the upload. ANSWERED in
PHASE 12b below (it does), which refutes the "the world ends at the 320x192 copy" reading above.

## PHASE 12b — THE STORE LANDS: THE 983,040-BYTE FRONT-BUFFER COPY CHANGES THE MIRROR EVERY SWAP, AND ITS SECOND COPY IS A NO-OP (2026-10-24)

Gate lowered (`trace_store_probe`: 1 MiB -> 512 KiB, command_processor.cpp:2680) and the probe's
window replaced (`resolve_call_count_ < 40` -> new `fh1_debug_resolve_store_probe_from` swap index,
160-probe budget): the front buffer's own copy is call ~12,000, so a first-40-resolves window could
never have seen it. Run p13 `20261024T072306Z-p13140` (GTX 570, hidden, executor=1 + readback=1 +
resolve_guest_refresh=1 + swap_fetch_trace=1 + store_probe_from=315): result.json `pass`, 1280x720
captures, vehicle moving 3600 -> 4200 (real free roam), 160 probe lines.

**Result: the earlier inference is refuted.** The 1CA95000/c0 resolve does land:

```
FH1 resolve store probe: call=7926 range=1CA95000+983040 rect=0,0x640,360 dest_base=1CA95000
  pitch=640 info=000E0050 (pack=0 endian=2 swap=1 ...) surface=0/16/1x/c0
  | mirror before=81B3078CE6B2BA6B/32159 after=192C643704E58C65/31505 ok=true changed=true
FH1 resolve store probe: call=7927 range=1CA95000+983040 ...
  | mirror before=192C643704E58C65/31505 after=192C643704E58C65/31505 ok=true changed=false
```

- 16 probed copies of that extent, **8 pairs, every pair `changed=true` then `changed=false`**
  (calls 7645/6, 7740/1, 7833/4, 7926/7, 8019/20, 8114/5, 8210/11, 8307/8): the first copy changes
  the mirror, the second identical copy is byte-exact - one resolve issued twice (predicated
  tiling), not a store that failed.
- ~51% of the stride-16 samples are nonzero (31,505/61,440): pixels, not a zero fill. The range
  just above the extent is all zero (`next_extent` hash with 0 nonzero samples), so the boundary is
  real.
- Call map (`fh1_debug_resolve_trace=318`): swap 318 = calls 7836..7927, so the world frame's own
  copy is probed at 7926. The same swap's depth copy (call 7845, 2,166,784 B) also changes the
  mirror (33,411 -> 9,196 nonzero samples).

Consequences. (1) The c3 2_10_10_10 copy (call 7909/12101) does **not** survive into the sampled
range: the two c0 copies follow it ~30 ms later in the same swap and overwrite all 983,040 bytes,
so "the world ends at the 320x192 copy" was wrong. (2) The fetch at 1CA95000 sees the c0 world
image itself - which is why R/B-restoring the t5 dump reproduces the c0 surface dump exactly - and
it looks garbled because the resolve applies `copy_dest_swap=1` (its info word has swap=1) while
this backend never applies the fetch's own swizzle 0x60A, so the pair no longer cancels and the
whole front buffer arrives red/blue-swapped. The composite target at the same swap
(`p12b-c2-draw4-top360.png`) holds a full world scene - trees, road, lamp post, headlights - in a
red/washed palette, i.e. the world is there, in the wrong channel order. (3) The base is still
genuinely shared in the guest's stream (depth 520x520, c3 320x192 2_10_10_10, c0 640x360 8_8_8_8,
every swap, same base), so a consumer that fetches the 245,760-byte range between the c3 copy and
the c0 copies gets the 2_10_10_10 image - the sampling order decides which one a draw sees.Still broken and separate: the *presented* frame of this run (`.local/diag/p13-roam3600.png`) is a
black band over the top ~55% with a blue fill and the HUD below, plus a rainbow strip on the
left edge, while the c2 composite target holds a full world scene at the same swap. That defect is
downstream of c2, not in 1CA95000, and the probe already localizes it to the front buffer's own
resolve: call=7847 `range=1CE2D000+3768320` (`720/16/1x/c0`, pitch 1280, swap=1) goes from
**11,311 nonzero samples to 4,413** of 235,520 (before=635BC3C7CE5DF9F6 after=127859752A3E7D39),
i.e. that dispatch writes ~98% zeros over the 1280x720 front buffer; the repeat at call=7848 is a
no-op. So the presented frame's black band is the front-buffer resolve's own output, and the next
question is why its source surface (or its source-by-owner fallback) is empty for those tiles -
not the swap readback.

## PHASE 11 — THE COMPOSITE'S FOOTPRINT IS THE WHOLE FRAME; THE READBACK WAS NEVER UNSTABLE; THE 48-VERT PASS IS A UNIT-SQUARE MESH, NOT A POST PASS (2026-10-24)

JOB 1: measure c2's true write footprint by a method proven stable first. ANSWER: both world
halves write their **entire 1280x360 rectangle** of c2 - no strip, no pitch/tile mismatch - and the
readback is stable to the bit (control 0.000000 on every probe). JOB 2: the 48-vert pass's own
vertices are a 2x2-cell mesh over the unit square (0..1), and the draw's clip transform is identity
(ndc scale 1,1, offset 0.00078,-0.00139), so it covers a quarter of the frame **by its own
geometry**; its measured writes are small boxes (255x255 px and smaller), so it is not the pass that
builds the presented frame. No fix is claimed and the acceptance frame was NOT produced.

### What was added (all default-off)
- `fh1_debug_c2_probe=<dir>` + `fh1_debug_c2_probe_from=<frame>` + `fh1_debug_c2_probe_draws=<n>`
  (src/graphics/d3d11/command_processor.cpp, flags.{h,cpp}): for each of the first n draws that write
  the front-buffer family (`base_tiles==0, pitch_tiles==16, 1x`) with a frame-width viewport
  (1280 x >=360) and a rectangle/triangle primitive, the target surface is read back **twice with
  nothing between** (the stability control) and once after the draw. One log line each, with the
  changed share, the change bounding box, and the change per 64-px column (20 deciles) and per
  60-px row (35 deciles over the 2048-row surface); the first six probes also write
  before/control/after PPMs. The readback
  is the dump's own staging+decode, refactored into `ReadTexture2DRgb` / `ReadSurfaceRgb` /
  `MeasureFootprint` (DumpTexture2DPpm and DumpSurfacePpm keep their exact behaviour).
- `fh1_debug_post_clip=<n>`: the post/frame-sampling pass's **own vertices** (up to 48 for its
  triangle list) with the fetched position attribute, the ndc each maps to, and the pixel it lands
  on, plus a one-line extent; it shares the c2 probe's frame window so it describes the same frames
  instead of the menus' first pass. Default 0; the vertex dump's original 3-vertex behaviour is
  unchanged when it is off.

### JOB 1 — THE CONTROL, THEN THE FOOTPRINT
Runs (GTX 570, hidden, monitor 2 config, executor=1 + readback=1 + resolve_guest_refresh=1):
- p11a `20261024T044608Z-p25744` - 40 probes, control only (the budget was spent by the HUD's
  line-list and glyph draws; the probe had no family/primitive filter yet).
- p11b `20261024T044816Z-p23568` - family+primitive filter; still reached the HUD's 320x192,
  160x96, 80x48 rectangle draws into the 1280-wide c3 surface.
- p11c `20261024T044957Z-p20764` - family+primitive+frame-width-viewport; 24 probes, the HUD's
  48-vert widgets included.
- p11d `20261024T045256Z-p28584` - 9 probes at 338-342 with the world/texture dump;
  p11e `20261024T045645Z-p11968` - 6 probes at 339-341 with the layer dump.

**STABILITY CONTROL: the readback is stable.** Every control (40 in p11a, 24 in p11c, 9 in p11d,
6 in p11e = 79) reports `control_changed=0.000000` with an empty bbox, i.e. two consecutive reads of the same
surface with no draw between them are identical to the bit. PHASE 10's inference that the surface
dump "is not stable between two consecutive reads" (from the p10j pairs reading 0.50 changed in
every band) is **retracted**: re-measuring that p10j pair directly gives a difference bbox of
exactly (0,0)-(1280,360) - the world half's own rectangle - with 0.1758 of the 1280x2048 surface
changed, i.e. 460,800 of 2,621,440 pixels = 1280x360 x 1.00: the draw wrote its entire half, and
the pair is a correct footprint, not an unstable read. A before/after pair without the control is
still not evidence - but with it, the pairs are.

**FOOTPRINT (p11d, swap 338, and p11e, swaps 339-341, 6/6 probes identical in shape):**
| probe | draw | viewport | changed | bbox | columns (20 x 64 px) | rows (33 x 60 px) |
|---|---|---|---|---|---|---|
| 1 | world half 0 | 0,0 1280x360 (3 verts, prim 8) | 0.500000 | 0,0-1279,359 | `55555555555555555555` | `999999000...` |
| 2 | world half 1 | 0,360 1280x360 (3 verts, prim 8) | 0.499997 | 0,360-1279,719 | `55555555555555555554` | `0000009999990...` |
Same numbers at 339, 340, 341: the draw changes **the entire 1280x360 rectangle of its own half**,
100% of those rows (decile 9 in every 60-px row of the half) and 50% of the pixels in **every one of
the 20 column bands** (the other 50% is where the new value equals the previous one within 8/255).
No probe shows a write confined to a 112-px band. (In p11c, probe 1 at swap 340 - a frame whose
previous state already held the strip and black - only 11.2% of the pixels *change*, and the bbox
of the change is 108..1171: the draw rewrote the same values over most of its half, which the diff
by definition cannot show. That is why the footprint claim rests on p11d/p11e, where the previous
frame differed across the whole rectangle.)

**c2 AS THE EXECUTOR HOLDS IT** (control log line, p11d probe 1):
`key=0/16/1x/c2` - base_tiles 0, pitch_tiles 16, MSAA 1x, format 2 (k_8_8_8_8) - `host=1280x2048`,
`res_fmt=28` (R8G8B8A8_UNORM), `view_fmt=28`, `samples=1`.
- width 1280 = 16 EDRAM tiles x 80 px: exactly the draws' 1280-px viewport, so the write and the
  surface agree on the pitch (a 1.6-tile-wide strip would need pitch_tiles 2, not 16).
- height 2048 = 128 tile rows x 16 px (Fh1SurfaceHeight), the EDRAM period for a 16-tile pitch; the
  frame uses the top 720 rows and rows 720..2048 are zero in every dump.
- The readback's mapped row pitch covers all 1280 columns of a 4-byte format (the dumps decode 1280
  full-width texels), so the host rows are 5120 B and no column is dropped on the way out.
- tiling is not involved on this leg: the surface is a linear D3D11 R8G8B8A8 texture written by the
  rasteriser; the guest's tiling applies to the *sampled* textures only.

**WHAT THE FRAME ACTUALLY CONTAINS AT THOSE SWAPS.** Same run, same swap, the layers the world
halves sample (p11e swap 340): t0 = 1C4E1000 1280x720 f54 - the world, full width, extremely dark
(mean 0.029: sky gradient, horizon, hills, a car at the left edge); t5 = 1CA95000 640x360 f6 -
**not a scene**: a yellow background with cyan/magenta line art (a UI/atlas-like image, 1728
colours, nonzero 1.0); t2 = 1DE5D000 320x192 f54 - **entirely black** (nonzero 0.0, 1 colour). The
composite's own output after the two halves is a full-width world with a hard seam at y=360
(`p11e-c2-340-top.png`: sky, hill, trees across all 1280 columns, red-tinted). So at these swaps the
composite writes and composes the **full width**, and c2's black middle at the frames where it
appeared follows the near-black scene texture of those frames rather than a confined write. What
the strip's *content* is (a squeezed layer or a bad layer) is not settled by these numbers.
The frame counter is not a stable frame index across runs: the same `swap=340` is the strip in p10h
and p11c and a full-width world in p11d/p11e, and the same captures (sim frames 3500/3700) show the
HUD screen in p10h and a HUD-less attract view in p11d.

### JOB 2 — THE 48-VERT PASS'S CLIP GEOMETRY
Run p11d, swap 347, draw #110 (`fh1_debug_post_clip=3`, 48 vertices, prim 4, vp 0,0 1280x720,
scissor 0,0 1280x720, ndc_scale 1,1,-1, ndc_offset 0.00078125,-0.0013888889,1):
- vertex fetch: slot 95, base 1411B8FC, stride 32, **format 37**, attribute offset 6, DMA indices
  (0,4,1,...), position attribute into r0.
- the positions form a 2x2-cell mesh over the unit square: `(0,0) (0.5,0) (1,0) (0,0.5) (0.5,0.5)
  (1,0.5) (0,1) (0.5,1) (1,1)` - 8 corner/mid values, 6 vertices per cell, 48 vertices total.
- mapped through the draw's own transform: px_x = [640.5, 1280.5], px_y = [359.5, 719.5] - the
  bottom-right quarter of its 1280x720 viewport (the transform is the identity, so [0,1] lands on
  the second half of NDC and the unit square maps to one quarter of the frame).
- **but the fetched attribute is not the VS's output**: the *measured* writes of the 48-vert
  prim-4 passes at swap 340 (same control) are 255x255 px or smaller - x 52..307 / y 352..607,
  x 1036..1209 / y 504..664, x 522..750 / y 658..689 - i.e. widgets, not the frame. Whatever the
  guest VS does with r0, these passes cannot build a 1280x720 image, and their input (the sampled
  frame) is full-width. The pass PHASE 10 called "the full-frame post pass" is a quarter-extent
  unit-square mesh; the quarter patch is its own geometry, not a rasteriser defect, and no fix in
  this leg can change the presented frame.

### WHERE THE BISECT STANDS
- **Convicted: nothing in the composite leg.** The composite writes the full 1280x360 rectangle of
  each half into a 1280-wide c2 whose pitch (16 tiles x 80 px) matches the viewport and the host
  texture; the descriptor is consistent with the draws' assumption; the readback is provably stable.
- **The strip is frame content, not geometry**: at the frames measured with the layer dump the
  scene texture is a full-width (very dark) world and the composed surface is full-width; t5
  (1CA95000, 640x360 f6) holds a garbled UI-like image and t2 (1DE5D000) is black at swap 340, so
  the layers the composite adds are themselves wrong even though its geometry is right.
- **The 48-vert pass is a quarter-extent unit-square mesh** (numbers above), not the frame builder.
- Not done: the acceptance frame (world at full width, the player's car on a road) - the captures
  of p11d (sim 3500/3700) show full-width worlds but a HUD-less attract view, not free roam, so
  they are not acceptance. No fix is claimed.

### Verified
- BUILD_EXIT=0 (`.local\muse_build_game.bat`).
- Default path unchanged: trace-short on defaults PASSes, **both captures md5
  `b8f68e89c6192577747ab452aa5598ef`** (run `20261024T045956Z-p10732`), the recorded baseline; every
  new diagnostic is behind `fh1_debug_c2_probe` / `fh1_debug_post_clip`, both empty/0 by default.
- Full gate suite green: `GATES_PASSED=4` (d3d11-cleared-frames, d3d11-texture-uploads,
  d3d11-native-resolve-path, d3d11-draws; .local/diag/p11_gates.out).
- Artifacts: .local/diag/c2p11 (before/control/after PPMs), .local/diag/texP11 (world/layer dumps),
  .local/diag/p11e-c2-340-top.png (the composed full-width frame), p11e-t5-340.png (the garbled t5),
  p11e-t0-340.png (the dark full-width scene), p11-strip-340-x4.png (the strip at 4x, p11c).

## PHASE 10 — THREE POINTS IN ONE SWAP: THE POST PASS'S INPUT IS FULL-WIDTH, ITS WRITE IS A QUARTER PATCH (2026-10-23)

JOB: capture the same swap at (i) the c2 surface after the two world draws, (ii) the resolve
destination in guest memory, (iii) what the post pass samples from 1DAC5000 - with the
format-matched lookup - plus the presented frame; report dimensions, per-64-px band means, and
which point first shows the squeeze. ANSWER: the post pass's input is a full-width world; the
pass's own write covers only x 0..320, y 360..720 of its 1280x720 viewport. The acceptance frame
(world at full width, car on a road) was NOT produced - no fix is claimed.

### What was added (all default-off)
`fh1_debug_texture_dump=<dir>` extended in src/graphics/d3d11/command_processor.cpp +
texture_cache.{h,cpp}:
- The world point is now only a draw that samples a >=1280x720 fetch (a `samples_scene_target`
  scan), so the loading screen's 640x360 halves no longer spend the budget; world and post have
  separate 300-dump budgets (`post=0` in earlier runs was the world spending all of it).
- `FindTexture2DForGuestBase` matches the *normalized* format (`rex::graphics::GetBaseFormat`,)
  - the same normalization `BindingInfoFromFetchConstant` applies - so a
  k_8_8_8_8_AS_16_16_16_16 fetch finds the k_8_8_8_8 texture the draw binds instead of missing it
  and falling through to a sibling at the same base.
- New `FH1 srv bind: swap=... slot=... base=... fetch=... -> key fmt=... ...` log in
  D3D11TextureCache::GetActiveTextureBindfulSRV: the binding path's own verdict for a frame-sized
  fetch (the key it binds, or "no valid binding"), gated on the same cvar. This is the answer to
  "which cache path decided it" that a pre-draw dump cannot give.
- New `after-<tag>.ppm` surface dump taken after the draw returns, and a `before-...-draw<N>.ppm`
  taken before each world-half draw, so a pass's own footprint is measurable as the difference
  between two dumps of the same surface.

### MEASURED
Run IDs: p10h 20261023T232804Z-p28160 (three points, swaps 339-352), p10i
20261023T233428Z-p16484 (post input + after-draw target, swaps 350-352), p10j
20261023T233752Z-p28704 (world before/after pairs). GTX 570, monitor 2, executor=1, readback=1,
resolve_guest_refresh=1, probe .local/probes/worldcomp.fh1test.

What the world draws sample (p10i, 300 dumps, normalized lookup; 106/106/88 occurrences):
| base | fetch | cache key | host | SRV |
|---|---|---|---|---|
| 1C4E1000 | 1280x720 f54 tiled | fmt 7 (2_10_10_10) | 1280x720 | 24 R10G10B10A2_UNORM |
| 1DE5D000 | 320x192 f54 | fmt 7 | 320x192 | 24 |
| 1CA95000 | 640x360 f6 tiled | fmt 6 (8_8_8_8) | **640x360** | 28 R8G8B8A8_UNORM_SRGB |
The scene texture is held at full 1280x720 under the format the fetch normalizes to, and the
`FH1 srv bind` log confirms the draw binds it (`slot=0 base=1C4E1000 fetch 1280x720 fmt=54 ->
key fmt=7 1280x720 texture=1 srv=1`); no frame-sized fetch reports "no valid binding".

T5 ANOMALY - SETTLED. The draw samples **1CA95000 as a 640x360 f6 texture** (host 640x360, SRV
R8G8B8A8_UNORM_SRGB). The 320x192 host texture at that base is a *different* cache entry
(the native shadow/depth-size sibling); only the unfiltered lookup returns it. PHASE 9D's "2:1
host-side reduction" was that sibling, not the drawn texture.

Point (iii) - the post pass's input (p10i swap 350): `slot 0 base=1DAC5000 fetch 1280x720 fmt=50
tiled=1 -> key fmt 6, host 1280x720`, SRV R8G8B8A8_UNORM_SRGB, nonzero **99.999%**, 25,049
distinct colours, mean 0.1268/0.5137/0.4172 (swap 351: 0.1269/0.5139/0.4200, 25,364). Per-64-px
band means over the full width: R 0.07 0.07 0.06 0.04 0.14 0.14 0.15 0.15 0.15 0.15 0.13 ... 0.14,
G 0.49 0.49 0.49 0.49 0.50 0.50 0.50 0.51 ... 0.52 - a sky gradient across the top, ground/road
below and trees at the left, with the horizon running the whole 1280 px. **The post pass's input
is a full-width world frame, not squeezed.** (Image: .local/diag/p10-postin-339-half.png, swap
339 of the same run family.)

This also retires PHASE 9D's post-pass lead: with the unfiltered lookup, base 1DAC5000 returns a
static fmt-7 entry that is a FORZA MOTORSPORT decal (identical mean 0.0227/0.0150/0.0177 and 268
colours across 15 swaps). With the base format the real 1280x720 f50/key-6 texture is found, and
it changes every swap. The "byte-identical post input" was the decal, not the front buffer.

Point (i) - the c2 surface after the world draws (p10h swap 339, `FindSurface(color0_key)`,
1280x2048): mean 0.1664/0.1723/0.1733, 2538 colours, nonzero 19.7%. Band means - rows 0..360:
0.58/0.35 at x=0/64, **0.00 from x 128 to 1152**, 0.28/0.51 at x 1152/1216; rows 360..720 flat
0.54..0.51 across the full width. Its column profile correlates with the presented capture at
r=0.917 (wc-3500) and 0.806 (wc-3700), so the presented layout is already fixed in this surface.
Stretching the populated left strip (x 0..90) to 1280 px (.local/diag/p10-leftstrip-stretch.png)
gives horizontal-stripe noise, not a scaled image: the strip is a **tiling/pitch artifact**, not a
15:1 squeeze of a scene.

What the post pass writes (p10i, before/after dumps of the same surface at swap 350, changed
fraction by 64-px band, threshold 8/255):
- rows 0..360: **0.00 in every band** (the pass does not touch the top half)
- rows 360..720: 0.09 at x 0, 0.43 at 64, 0.48 at 128, 0.38 at 192, 0.28 at 256, **0.00 from
  x 320 to 1280**
So the 48-vertex pass, with a 0,0 1280x720 viewport and ndc scale 1,1 offset 0.00078,-0.00139,
rasterizes into a ~320x360 patch at the bottom-left - a quarter of its viewport - instead of the
frame. Its target after the draw is 19.0% nonzero, mean 0.15, 3102 colours.

### Where the bisect stands
- The scene texture, the composite's drawn fetches and the post pass's sampled frame are all
  full-width. No input in this chain is squeezed.
- The squeeze/strip in the presented frame is already present in the c2 surface (r=0.92 with the
  capture) and its content is a tiling artifact rather than a scaled image.
- The post pass does not replace the frame: it writes a quarter patch. Whatever writes the
  presented frame, this pass is not it.
- Convicted leg on the evidence: **the front-buffer composition into c2 (leg a) for the strip,
  with the post pass (leg c) also failing to cover the frame** - two defects, not one. The next
  measurement must be the composite's own footprint in a way the surface dump can be trusted for
  (the p10j before/after pairs of world draws came out 0.50 changed in *every* band, i.e. the
  surface dump is not stable between two consecutive reads, so that pair is unusable).
- Not done: the post pass's clip-space geometry (guest VS, fmt 57 world-space template; the r0
  heuristic stays dead), and therefore no fix and no acceptance capture.
- Tooling gap found: `fh1_debug_surface_dump` / `fh1_debug_dump_guest` never fire under this
  probe. `DumpNativeSurfacesOnce` is only called from the presenter path in `IssueSwap`, and a
  render-test run with `capture_source=guest_output` presents through the guest output image
  (one `IssueSwap` line in the whole run), so neither a whole-surface dump nor the guest-memory
  dump of the resolve destination is reachable there. Point (ii) was therefore taken as the post
  pass's sampled texture, which is that same guest memory uploaded by the cache.

### Verified
- BUILD_EXIT=0 (.local/muse_build_game.bat).
- Default path unchanged: trace-short on defaults PASSes, captures md5
  `b8f68e89c6192577747ab452aa5598ef` for both captures (run 20261023T233925Z-p25988), the
  recorded baseline; every new diagnostic is behind `fh1_debug_texture_dump`.
- Artifacts: .local/diag/p10h.log (three points 339-352), .local/diag/p10i.log (post input and
  before/after 350-352), .local/diag/texH, .local/diag/texI, plus the PNGs listed above.

## PHASE 9D — THE SCENE TEXTURE IS FULL-FRAME: THE SQUEEZE IS DOWNSTREAM (2026-10-23)

JOB: dump the 1280x720 f54 scene texture (t0 = 1C4E1000) as the composite samples it, and say
whether the world is already squeezed into a strip before the composite draws it. ANSWER: NO -
the texture holds the world across the full 1280x720; the strip appears after it.

### What was added
`fh1_debug_texture_dump=<existing dir>` (string, default empty), in command_processor.cpp +
texture_cache.{h,cpp}: at the world rectangle draw whose viewport is 0,0 1280x360 (and at the
full-frame 1280x720 triangle-list post pass), every sampled texture of at least 320x192 is read
back from the host texture the cache holds and written as `<dir>/tex-<swap>-t<index>.ppm`, with
its guest size/format/tiling, host size, SRV format, mean RGB and colour count. The readback is
the shared `DumpTexture2DPpm` (the surface dump's staging + decode, now factored out). The cache
lookup `FindTexture2DForGuestBase` matches the fetch's *format* as well as its base, because one
base can have several format siblings and the first in the multimap is not what the draw samples.

### MEASURED (run texdump7, swaps 342-348, GTX 570 hidden, executor=1 + readback=1 +
fh1_resolve_guest_refresh=1; probe worldcomp.fh1test)
World draw (#68/#84/#100/#116, vp 0,0 1280x360) samples: t0=1C4E1000 1280x720 f54 tiled,
t2=1DE5D000 320x192 f54, t5=1CA95000 640x360 f6, t7=136FB000 6160x7684 f6, t17=1FCA6000 32x32 f36.
- t0: host 1280x720, SRV fmt 24 (R10G10B10A2_UNORM), mean RGB 0.046,0.041,0.032, 184-582
  distinct colours. **The image is a complete frame of the world**: sky gradient across the whole
  width, hills and road across the bottom, trees at the left - no narrow column, no letterbox
  (per-64px column band means 0.037-0.046 with structure in every band, max 0.051-0.066). It is
  stored very dark (~4% of full scale), which is expected of the linear scene target.
- t2: host 320x192, mean 0.053-0.069 - the same scene at quarter resolution.
- t5: guest 640x360 f6 but **host 320x192** (a 2:1 host-side reduction), mean 0.128-0.189 - the
  brightest layer, and worth its own look: the composite may be sampling the scene at half size.
- t7 (6160x7684 untiled) has no host texture in the cache (reported, not dumped).

### Where the squeeze is, therefore
Same run's presented frame (wc-3700): the only structure is x 1152..1280 (64-px band means 0.236
and 0.349, against a flat 0.12-0.15 for x 256..1152), i.e. the world occupies 128 of 1280 columns
- a 10:1 horizontal compression of the full-frame scene that sits in t0. Since the two world
draws were measured in PHASE 9C as full-viewport quads with the correct half-frame UVs, and their
input texture is correct, the compression is introduced by (a) the composition into the c2
surface, (b) the resolve of c2, or (c) the full-frame post pass that samples 1DAC5000 back.

### Not yet established (do not treat as a finding)
The post pass (prim 4, 48 verts, vp 0,0 1280x720, samples t0=1DAC5000 1280x720 f50 and
1=14166000 128x128 f59) was dumped in one run with the *unfiltered* cache lookup and read
byte-identical across 8 swaps (mean 0.0227,0.0150,0.0177, 268 colours, 3.5% nonzero) - but the
lookup could have returned a stale format sibling at that base, so this is a lead, not a result.
The format-matched rerun ended at presented frame 330, before the trace start, so it did not
reach the post pass. The presented-frame count at stop varies 330-370 between runs - do not pin a
trace start to 340 without checking.

### Verified
- BUILD_EXIT=0. Default path unchanged: trace-short on defaults PASSes, captures md5
  `b8f68e89c6192577747ab452aa5598ef` (the recorded baseline), so all new work is inert with the
  diagnostic cvars unset.
- Images: `.local/diag/tex/tex-348-t0.png` (raw) and `tex-348-t0-x6.png` (same, brightness x6 for
  inspection), plus t2/t5 of the same swap.

## PHASE 9C — THE WORLD DRAWS' RECTANGLES: MEASURED, AND THEY ARE CORRECT (2026-10-23)

JOB: finish the vertex-fetch dump, then report the three rectangle corners of the two world draws
against their 1280x360 viewports. DONE, and the answer rules the rectangle path out.

### The dump is now correct (the 9B version was not)
Written in src/graphics/d3d11/command_processor.cpp (`fh1_debug_vertex_dump`, default 0). What the
9B pass got wrong and this one fixes:
- `xe_gpu_vertex_fetch_t`: `dword_0` is `type:2 | address:30` with **address in dwords**;
  `dword_1` is `endian:2 | size:24` with size in dwords. The dump now reads
  `vf.address * 4` and reports `vf.size * 4`.
- The vertex is **base + stride * index + attribute offset**, not the base: one address per guest
  vertex, per binding attribute, with `binding.stride_words` (the translator sets it to the fetch
  instruction's stride) and `fetch_instr.attributes.offset` (signed, dwords).
- The index is the draw's own: auto-indexed draws use `(i + VGT_INDX_OFFSET) & 0xFFFFFF` (what the
  translated VS computes in RemapAndConvertVertexIndices), DMA draws read the index buffer at
  `IndexBufferInfo::guest_base` with the draw's index size **and endianness** (`xenos::GpuSwap`) -
  without the swap the first triangle of the post pass read as 0,1024,256 instead of 0,4,1.
- Dwords are endian-swapped with `vf.endian` like the shader's own fetch; fmt 36/37/57 attributes
  print as floats, the rest as hex; the attribute that writes r0 gets the viewport mapping
  `px = (ndc * ndc_scale + ndc_offset + 1) * 0.5 * extent + offset`.
- Gated on `same_family` + a frame-sized sampled texture, like the trace it hangs off; 9B ran it for
  every frame-texture draw and spent the budget before the world phase (and logged draw#1
  repeatedly, because `composite_draw_logs` only counts front-buffer-family draws).

### MEASURED: the two world draws (run vdump8 swap 334, same vertices at swap 330 in vdump4)
Probe `.local/probes/worldcomp.fh1test`; executor=1, readback=1, fh1_resolve_guest_refresh=1,
`fh1_debug_composite_draws=320`, `fh1_debug_composite_world_only`, `fh1_debug_vertex_dump=160`.
Both draws: prim 8 (rectangle list), 3 guest vertices, auto-indexed, index offset 0 (indices 0,1,2),
fetch constant 95, buffer 48 B, stride 16 B, position = fmt 37 (k_32_32_FLOAT) at offset 0, UV =
fmt 37 at offset 2 - i.e. one `{float2 pos; float2 uv;}` vertex array, 3 vertices.

  draw#103  vp 0,0 1280x360   base 1FCA3000
    v0 (-1, 1) -> px (0, 360)      uv (0, 0)
    v1 ( 1, 1) -> px (1280, 360)   uv (1, 0)
    v2 (-1,-1) -> px (0, 0)        uv (0, 0.5)
    4th (v1-v0+v2) = (1,-1) -> px (1280, 0), uv (1, 0.5)
    => covers exactly x 0..1280, y 0..360 = the full viewport; samples the TOP half of the scene.
  draw#104  vp 0,360 1280x360   base 1FCA2000
    v0 (-1, 1) -> px (0, 720)      uv (0, 0.5)
    v1 ( 1, 1) -> px (1280, 720)   uv (1, 0.5)
    v2 (-1,-1) -> px (0, 360)      uv (0, 1)
    4th = (1,-1) -> px (1280, 360), uv (1, 1)
    => covers exactly x 0..1280, y 360..720; samples the BOTTOM half.

The rect-list corner rule (spirv_translator.cpp:1276, the same construction the DXBC path shares):
the quad is x from v0/v1, y from v0/v2, and the synthetic fourth corner is `v1 - v0 + v2`. So all
the y values of v0 and v1 being +1 does NOT make a degenerate rect - 9B's reading of the same
numbers as "zero height" was wrong, and the two draws together cover the whole 1280x720 exactly
once with UV 0..1 split at 0.5. **The composite's rectangle geometry and UV split are correct.**
Consequence: the world squeezed into the right ~110 px of the presented frame cannot come from
these draws' corners or UVs. The remaining candidates are (a) the content of the 1280x720 f54
scene texture t0=1C4E1000 itself, and (b) the final post pass #105 (prim 4, 48 verts, DMA-indexed,
samples t0=1DAC5000:1280x720:f50) that overwrites the composed halves.

### The post pass is NOT measurable with the r0 heuristic (recorded, so it is not re-tried)
#105's bindings are fmt 57 (k_32_32_32_FLOAT) at offset 0 - a world-space template `(-50,-50,0),
(0,0,0), (-50,0,0)` with a normal at offset 3 - and its r0 attribute holds `(0,0),(0.5,0.5),(0,0.5)`,
not a clip-space position. The viewport mapping printed for it is therefore meaningless; its
clip position is produced by the guest VS, not fetched. Measuring that pass needs the shader's
transformed output (or its constants), not the vertex buffer.

### Verified
- BUILD_EXIT=0 (.local/muse_build_game.bat).
- Default path unchanged: trace-short on defaults PASSes, captures md5
  `b8f68e89c6192577747ab452aa5598ef` - byte-identical to the recorded baseline (all new work is
  behind `fh1_debug_vertex_dump`, default 0).

## PHASE 9B — THE COMPOSITE'S WORLD DRAWS: MEASURED, STILL NOT COMPOSED RIGHT (2026-10-23, in progress)

JOB: compose the 3D scene into the front buffer (continuation prompt). STATE: the residency chain
fix from PHASE 9 is in and proven; the composition defect that remains is now localised to two
specific draws, and the acceptance battery is NOT met. Nothing in the default path changed
(every new diagnostic is default-off; fh1_resolve_guest_refresh is still default-off).

### What the fix does now, on the world frame (measured, this pass)
Probe `.local/probes/worldcomp.fh1test` (the free-roam entry taps, captures at 3500/3700, stop
3800 - the world is reached at ~340 presented frames on the executor path, i.e. guest frame ~3500).
Runs: worldcomp-20261023T184027Z .. 185826Z, 190322Z (GTX 570, hidden, executor=1 + readback=1,
fh1_resolve_guest_refresh=1).
- Captures with the fix on: `wc-3500` mean 74.195, `wc-3700` (see .local/diag/wc-3700.png) is no
  longer a flat sky - it carries a real scene strip, but the frame is still wrong (below).
- The presented frame (wc-3700, 1280x720): top half a flat saturated blue, bottom half a light
  blue/white gradient with the HUD (speedometer, PRESS A TO ACCELERATE, SAVING, song toast, 2.6 MI),
  a red/pink dithered column at x 0..190, a 256x256 diagonally striped patch at (50,360) with a car
  silhouette in it, and a coherent hills/snow/trees strip at x 1170..1280. So the world IS being
  sampled now; its mapping onto the composite surface is what is wrong.

### The composite's own draw sequence in the world phase (measured, fh1_debug_composite_draws +
fh1_debug_composite_world_only, run 190322Z swap 350)
Eight frame-sized-texture draws per swap, all into the front-buffer family, repeating every swap:
  #146  c3  vp 0,0 320x192   t0=1DAC5000:1280x720:f6   (depth downsample)
  #147  c0  vp 0,0 640x360   t2=1DAC5000:1280x720:f6   (half-res depth)
  #148  c0  vp 0,0 640x360   t3=1CA95000:640x360:f6
  #149  c2  vp 0,0 1280x360  t0=1C4E1000:1280x720:f54 t2=1DE5D000 t5=1CA95000 t7=136FB000:6160x7684
  #150  c2  vp 0,360 1280x360 (same textures)          <- the scene's two half-frame draws
  #151  c2  vp 0,0 1280x720  t0=1DAC5000:1280x720:f50 t1=14166000:128x128  (full-screen post)
- #149/#150 are `prim=8` (kRectangleList) with **verts=3/3** - one rectangle each, expanded by the
  rectangle-list geometry shader into a quad - and `shader=0` (kVertex, no tessellation), while the
  full-screen post pass #151 is `prim=4` (triangle list) verts=48/48. The NDC system constant for
  the two world draws is scale (1,1), offset (0,-0), identical to the HUD's 2D draws, so the
  projection constant is not the difference between the correct HUD and the wrong world quad.
- Every one of them is `same_family` (base_tiles 0, pitch 16, 1x): c0/c1/c2/c3 are *format siblings
  sharing the same EDRAM tiles*, which is why a surface dump of one key shows another key's pixels
  (see below) - the pixel dump of a single key is not a valid picture of a pass in this frame.

### What the surface dumps can and cannot say (measured, and corrected here)
fh1_debug_composite_dump_world (new) dumps the front-buffer source surface before each traced
frame-sized-texture draw; fh1_debug_clear_draw clears it to magenta every other dump so one dump
shows a single draw's footprint. `.local/diag/wd/world-before-*.ppm` (run 185826Z):
- The isolated footprints all come back as 640x360 at the origin (bbox 0..636 x 0..364) for every
  c2 draw, and the content inside is a complete half-scale scene (road, car, trees, hills).
- That is NOT the world draw at half size: 640x360 is exactly the c0/c2 half-resolution
  intermediate pass (#147/#148), and because c0/c2 share EDRAM tiles a c2-view dump of the region
  shows that sibling's pixels. Conclusion, recorded so the next pass does not re-run it: a key
  dump in this family cannot isolate a draw's output; the presented frame (the front-buffer
  resolve) is the only trustworthy picture.
- fh1_debug_vertex_dump (new, 48 dwords of each used vertex fetch slot) returned zeros at the
  logged addresses on the world draw (`addr=0059E4FC`, `007FE3FC`, `0017E13C` with `d[0]`, so the
  dump read the wrong guess at the fetch address - the fetch-constant address/size packing was not
  reproduced correctly). The rectangle's own coordinates are still UNMEASURED; the probe needs the
  address/size fields decoded properly (xe_gpu_vertex_fetch_t: type:2, address:22 in dwords,
  size:24 - the dump must use `(dword_0 >> 2) & 0x3FFFFF` dwords, not the raw low 32 bits, and
  read from the mirror, not guest memory, so a GPU-written page cannot blank it).

### Where the next pass should look (ranked, with the reasons the earlier list is now exhausted)
1. The rectangle-list path for these two draws: they are the only frame-filling draws in the frame
   that are a *rectangle list* (3 vertices, one rect) AND sample a 1280x720 texture with
   `point_coordinates`-based UVs, while every other correct 2D draw (HUD) is a small rect. Measure
   the three guest vertices (fix the vertex dump above) and compare the resulting quad against the
   guest viewport (0,0 1280x360 / 0,360 1280x360). If the quad is right, the defect is the UV
   generation for the rect-list path (`has_point_coordinates` / the texture coordinate scale in
   the pixel shader), which is also where the diagonal striping in the 256x256 patch comes from.
2. The 256x256 patch at (50,360) and the left/right edge strips: they appear only with the fix on,
   so they are draws that sample the freshly-landed scene and land at the wrong place - the same
   rect-list UV question, or the c0 256x256 draw the earlier phase already logged into c0.
3. Cost, still open: one GPU wait + readback per texture load (median 181 ms, 4.75 Hz distinct
   presentation vs the 5 Hz floor). The landing has to be batched per swap or moved off the CPU
   before `fh1_resolve_guest_refresh` can default on.

### Housekeeping
- New cvars, all default-off, no behaviour change when unset (src/graphics/flags.{h,cpp}):
  fh1_debug_composite_dump_world (string), fh1_debug_composite_world_only (bool),
  fh1_debug_clear_draw (int32), fh1_debug_vertex_dump (int32). The composite draw log now also
  prints prim type, guest/host vertex counts, shader type and the NDC scale/offset, which is what
  named the two draws above.
- BUILD: .local/muse_build_game.bat -> BUILD_EXIT=0 (log .local/muse_build_game.log).
- The graded scenario fh1-free-roam currently FAILS on its capture-uniqueness assertion on
  *defaults* (roam-4200 vs roam-5400 MAE 0.049, and in an earlier run roam-3600 vs roam-4800
  MAE 9.25) - because both frames are sky-dominated load/world-entry frames whose content depends
  on wall-clock pacing. This is not caused by any change in this pass (the refresh is default-off
  and the diagnostics are inert), but it means the scenario cannot pass 4/4 until it captures
  frames that actually contain the world (its captures at 3600-4800 land on the world entry, not
  the open world the probe reaches at 7000+).

## PHASE 9 — THE WORLD REACHES THE COMPOSITE: RESOLVES NEVER LANDED IN GUEST MEMORY (2026-10-23, in progress)

JOB: compose the 3D scene into the front buffer (continuation prompt).
VERDICT SO FAR: the drop is CONVICTED and the chain fix is in - a resolve writes the GPU MIRROR of
shared memory, and the D3D11 texture cache untiles from GUEST memory, which the resolve never wrote.
Every texture the guest resolved and then sampled in the same frame failed its load: measured, not
inferred (below). With the fix the world texture loads and the composite draws it. The frame is not
right yet (top half black, a stray 256x256 patch); that is the NEXT defect and is being bisected now.

BISECT 4 — WHY THE COMPOSITE'S TEXTURE WAS EMPTY (probe run roam-dump-20261023T171902Z,
executor=1 + readback=1 + fh1_swap_fetch_trace):
  `FH1 texture load rejected: base base=1C4E1000+3768320 1280x720x1 format=7 tiled=1 pages
   valid=920 gpu_written=920` on every composite frame - the three-chunk 3D resolve's own
destination (1C4E1000, 1280x720, k_2_10_10_10), all 920 pages GPU-written. `CopyCpuRange` refuses
GPU-written pages, so `LoadTextureDataFromResidentMemoryImpl` returned false and the host texture
stayed empty; the composite drew the world quad and sampled nothing. Same for the shadow/depth
resolves (1CA95000 320x192, 1CAD1000 64x32...) and for 1D728000 1280x720 format=6. The 2D/HUD and
the sky fills use no texture at all (`textures=none`), which is why they composed correctly.

FIX (chain-level, general - no scenario or free-roam special casing):
- `SharedMemory::CopyResolvedRangeToGuest` (src/graphics/shared_memory.cpp): writes the bytes the
  GPU produced into guest memory and clears the GPU-written flag of each page it lands in, so the
  page is CPU-authoritative again (valid, CPU write watch still armed) and a texture upload may
  copy it. Pages the CPU wrote since the resolve are left alone.
- `D3D11CommandProcessor::RefreshResolvedGuestRange` (command_processor.cpp): if any page of a
  range is GPU-written, waits for the GPU, reads the range from the mirror
  (ReadbackFromHostBuffer) and lands it through CopyResolvedRangeToGuest. Early-outs (no wait, no
  copy) when no page is GPU-written.
- `D3D11TextureCache::LoadTextureDataFromResidentMemoryImpl` calls it for the base and the mips
  range before `CopyCpuRange` - exactly where a resolved texture's pixels can come from in this
  backend. The swap's own readback (fh1_d3d11_resolve_readback) is unchanged.

MEASURED, FIRST RUN WITH THE FIX (roam-dump-20261023T172544Z, GTX 570 hidden, executor=1 +
readback=1 + trace, dump at swap 700):
- `FH1 resolve refresh: range=1C4E1000+3768320 pages gpu_written_before=920 after=0` on the first
  composite frame, then 1CA95000 (60), 1CAD1000 (2,1), 1FCA6000 (1), 1DE5D000 (60) ...; texture
  load rejections: 0 for the rest of the run (they were ~3 per frame before).
- Captures change: world-a/b/c means 83.489 (frozen, identical md5) -> 48.952 / 64.491 / 64.495,
  i.e. the presented frame now follows the simulation.
- The presented frame is still wrong: rows 0-360 pure black, rows 360-720 a blue gradient with the
  HUD, plus a 256x256 striped patch of world at (50,359). Before the fix that area was two flat
  blue bands. So the world quad now samples real data but what it produces is not the scene.

BISECT 5 — THE COMPOSITE'S OWN DRAWS (fh1_debug_composite_draws=500, run roam-dump-20261023T175129Z):
400 lines, the draws that write 0/16/1x/c2 / c0 / c3 in the world phase:
  #361 c2 vp=0,0 1280x360   t0=1C4E1000:1280x720 (the world texture, top half of the screen)
  #362 c2 vp=0,360 1280x360 t0=1C4E1000:1280x720 (bottom half)
  #363 c2 vp=0,0 1280x720   t0=1DAC5000:1280x720:f50  (a full-screen post pass)
  #364/365 c2 full screen   t0=1417D000 / 14195000 256x256 (bloom)
  #389+ c2 full screen      t0=1417C000:8x8 (colour grading LUT)
  the rest: 0/16/1x/c0 full-screen draws with no texture (the sky/fills), and 256x256 draws into c0.
So the world IS drawn in two 1280x360 halves and then post-processed. The black top half is produced
by one of the passes above, and the 256x256 patch at (50,359) is a c0-space 256x256 draw (vp 0,0
256x256) that the c0->c2 ownership transfer lands at an offset - both under bisect, see NEXT.

BISECT 6 — THE FRAME WITH THE FIX ON, AT SWAP 500 (same run family; guest extents and the composite
target dumped at the swap):
- Guest memory at 1C4E1000 untiles (GetTiledOffset2D, pitch 1280, k_2_10_10_10) to a coherent
  1280x720 scene - sky, hills, a sign, trees, terrain. 942,080/942,080 words non-zero, 3,192
  distinct. The resolved texture is NOT the defect any more.
- Every composite input has content: 1D728000 1280x720 (479,615/942,080 words), 1DAC5000 1280x720
  depth, 1DE5D000 320x192 bloom mip (61,440/61,440), 1CA95000 320x192, 15FBF000 512x512.
- The presented target (0/16/1x/c2) is still wrong: rows 0-359 = exactly 0 (pure black), rows
  360-719 = a smooth blue gradient plus the HUD and a 256x256 striped patch at (50,359).
- The world is drawn by two full-frame-width draws (vp 0,0 1280x360 and vp 0,360 1280x360, both
  sampling 1C4E1000), followed by post passes sampling the depth, the bloom mips and an 8x8 LUT.
- MEASURED ORIENTATION: the frame's bottom half correlates with the world texture's TOP half
  (row-mean correlation +0.52 for the vertical flip, -0.49 unflipped; blurred pixel correlation
  0.217 vs 0.030 for the matching half) and the top half is exactly zero. So the composite draws
  the resolved scene INVERTED and only its sky half survives - a chain defect downstream of the
  residency fix, not the residency fix itself.

COST, MEASURED (fh1-free-roam, GTX 570, executor=1 + readback=1, refresh on): median 181.2 ms (was
~108-115 ms), distinct presentation 4.748 Hz (floor 5.0) - the scenario FAILS both, because every
landing waits for the GPU and reads the range back per texture load (the world texture alone is
3.7 MB, the depth target 7.4 MB). The refined build therefore keeps the path behind
`fh1_resolve_guest_refresh` (bool, DEFAULT OFF, restart): the data landing is proven correct (920
of 920 GPU-written pages become CPU-authoritative, and the load refusals drop from ~3 per frame to
0), but it cannot default on until the flush is batched or the untiling leaves the CPU.

NEXT (in the order the evidence ranks it):
1. The composite's world draws: why only the bottom half is written and why it samples the texture
   flipped (two draws with adjacent viewports, same texture - check the 2D vertex path's window
   offset and V orientation against the HUD's draws, which are upright).
2. The 256x256 patch at (50,359): a 256x256 viewport draw into 0/16/1x/c0 (sampling 15FBF000
   512x512) that reaches the presented surface at an offset through the alias transfer.
3. Then make the residency path affordable: one GPU wait per swap (batch the resolved ranges, land
   them next to the existing frontbuffer readback) instead of one per texture load, and measure
   free-roam again against 150 ms / 5 Hz.

## PHASE 8 — WORLD-CHAIN BISECT: THE SCENE IS IN c3, THE PRESENTED SURFACE IS NOT (2026-10-23, in progress)

JOB: compose the 3D scene into the presented frame (Freebuff-Pinyon-WorldChain-Prompt.txt).
STATE: instrumentation written and built (BUILD_EXIT=0), first measurements taken; NO FIX YET.
The bisect CONTRADICTS the prompt's premise in one important way (recorded, not patched around):
in the world phase the front buffer does NOT resolve from 0/16/1x/c10. c10 resolves exist only
at swaps 31-458 (300 of them, the load screen); from swap 459 on the presented copy's source is
0/16/1x/c2. The measured drop is upstream of the c10 identity.

WHAT WAS WRITTEN (diagnostic only, default-off; no behaviour change with the cvars unset)
- src/graphics/flags.{h,cpp}: fh1_debug_surface_dump (string, directory, default "") and
  fh1_debug_surface_dump_swap (int32, default 300).
- include/rex/graphics/d3d11/fh1_native_surfaces.h: surfaces() accessor over the registry.
- src/graphics/d3d11/command_processor.{h,cpp}: DumpNativeSurfacesOnce() - at the first swap >=
  fh1_debug_surface_dump_swap, after WaitForGpu, dump every registered native surface as a PPM
  (MSAA resolved to 1x, decoded per typed format: R10G10B10A2_UNORM, R8G8B8A8_UNORM,
  R16G16B16A16_FLOAT/UNORM) plus key/size/samples/drawn/nonzero-share/colours/mean, and the
  EDRAM ownership histogram (owner key -> tiles). Also added src=<key> by_owner=<0/1> to the
  "FH1 resolve key" trace line and last_resolve_source_key_.
- .local/probes/roam-dump.fh1test: the free-roam entry taps with captures at 7000/9000/11000 and
  stop 12000, because fh1_swap_fetch_trace slows the guest so much that the graded scenario's
  5,460 frames never reach the world.
BUILD: .local/muse_build_game.bat -> BUILD_EXIT=0 (log .local/muse_build_game.log).
RUN: roam-dump-20261023T170232Z (GTX 570, hidden, executor=1 + readback=1 + swap trace,
fh1_debug_surface_dump=.local/diag/surfaces, dump swap 700; --no-performance-gate because the
trace raises the median to 154 ms). Dump artefacts: .local/diag/surfaces/surfaces.txt +
surface-*.ppm; converted views .local/diag/front-c2-1x.png, .local/diag/scene-c3-32-4x.png.

BISECT 1 — END-OF-FRAME CONTENT PER SURFACE (swap 700, 36 surfaces; PPMs are 1x views):
- 0/32/4x/c3  1280x512x4  drawn=1  nonzero=52.80% colours=2567  <- THE WORLD IS HERE (forest,
  terrain, hills visible in .local/diag/scene-c3-32-4x.png; its bottom half is black+noise)
- 0/16/1x/c3  1280x2048x1 drawn=1  nonzero= 4.14% colours= 563
- 0/16/1x/c2  1280x2048x1 drawn=1  nonzero=35.09% colours= 694  <- THE PRESENTED SURFACE: sky +
  HUD only (see .local/diag/front-c2-1x.png; the dump decodes R and B in the wrong order so the
  sky reads red, the structure is the point: HUD text/dial exact, flat sky elsewhere)
- 0/16/4x/c2  640x1024x4   drawn=1  nonzero= 0.00% colours=   1  (the 4x HDR sibling: EMPTY)
- 0/16/1x/c1  1280x2048x1 drawn=1  nonzero=35.16% colours=   2  (a fill, two colours)
- 0/16/1x/c0  0.24%, 0/16/4x/c0 9.44%/2099, 720/16/4x/c0 14.82%/2075, 0/5/4x/c0 12.73%/2309,
  0/4/2x/c3 14.46%/964, 0/16/4x/c3 7.35%/568, 0/16/2x/c12 0%/1 drawn=0.
- OWNERSHIP at the same swap (all 2048 tiles): 0/16/1x/c3 = 1328 tiles and 0/16/1x/c2 = 720 tiles.
  NOTHING ELSE OWNS A TILE - in particular 0/32/4x/c3 owns ZERO tiles, so the surface that holds
  the world image is not the surface the frame's draws filled, and the 4x chunks are read from a
  stale surface (its content is still a real world frame, which is why fragments appear).

BISECT 2 — PACKET SEQUENCE PER SWAP IN THE WORLD PHASE (swaps 700-703, trace):
  depth resolves: 720/13/1x/d0 (shadow maps), 0/16/1x/d1 (frontbuffer-sized depth)
  colour: 720/16/1x/c0 (full frame, twice, 170/495 and 178/497 nonzero sampled texels)
  3D chunks: 1024/32/4x/d1 + 0/32/4x/c3 at rects 0,0-1280,256 / 0,0-1280,256 / 0,0-1280,208 to
    distinct guest extents 1C4E1000, 1C621000, 1C761000 (+1,310,720 B each, dest t7 = 4 B/texel)
  then 0/16/1x/c0 256x256 (0x0010 info) and finally 0/16/1x/c2 0,0-1280,720 -> the front buffer
    (extent 1BDB1000/1C149000+3,768,320, info 000E0041).
  EVERY one of those resolves reports by_owner=0 and src=<its own key>: ResolveSourceOwner never
  redirects in the world phase. For the 4x chunks that is by design (its msaa guard refuses a
  rectangle owned by a 1x surface), so the stale-4x read is the mechanism above, not a redirect.
  All three chunk rects start at y=0 with different destinations: the guest's predicated tiling
  names a different destination base per chunk, so that is not itself the defect.

BISECT 3 — rt_bound AND c10: 0/16/1x/c10 resolves 300 times, swaps 31-458 only (load screen), and
no 0/16/1x/c10 surface exists in the swap-700 registry at all. In the world phase the presented
copy is c2->t7/1x/s0 with rt_bound=1 (a real draw target) and the c2 surface is the sky+HUD
composite. So the c10 identity is a load-screen phenomenon on this build; the world-phase drop is
between the scene target (0/32/4x/c3, no longer owned) and the composite target (0/16/1x/c2).

NEXT (not yet done, in the order the evidence ranks it):
1. Per-frame draw-target histogram (draws per RT key, top N, at the dump swap) to name the surface
   the world's geometry actually binds in the world phase - the ownership histogram says 0/16/1x/c3,
   but c3 1x holds only 4% content, so either the world draws bind it and write little, or they
   bind another key and the ownership moved afterwards.
2. Check the 3D chunk resolve OUTPUT (staging words_nz / a dump of the mirror extent at 1C4E1000)
   against the c3 4x source: is the world image carried through the resolve at all? The staging log
   is capped at 600 lines early; raise the cap or add a dump of that extent.
3. Then fix at the convicted point (most likely: the scene target identity the draws bind vs the
   identity the copies name - the format-sibling/ownership class), and run the acceptance battery.

## PHASE 7 — FREE ROAM CAPTURES THE WORLD, NOT THE LOAD SCREEN (2026-10-23, this pass)

VERDICT: fh1-free-roam now presents and captures FH1's open world, and it passes with the
executor + readback path (result pass, 4 captures in game_mode 17). Two facts came out of it:
(1) the blue frames this scenario used to capture were the open-world LOAD SCREEN, not a broken
world - the world arrives at ~3500 output frames; (2) the world frame's 3D content is still
composed wrong: the HUD is pixel-correct, the world is 88-96% flat sky with a few mis-placed
strips. That second item is a renderer defect and stays OPEN.

WHAT WAS WRITTEN (config/render-tests/fh1-free-roam.fh1test, timeline + expectations only)
- Captures moved from 1200/1500/1800 (probe-*) to 3600/4200/4800/5400 (roam-*) and the stop from
  1830 to 5460. Same eleven input steps, no new waits: the load is time-based here.
- Expectations the load screen cannot satisfy: expect-distinct-presentation 5,
  expect-capture-mae roam-3600 roam-4800 10, expect-capture-mae roam-4200 roam-5400 10,
  expect-performance 150000 5 10 30 (median us, present Hz, simulation-tick band),
  expect-simulation-time 0.95 1.08 2. The old 50000/55 floor was removed: the open world runs
  at 10 Hz on this adapter, and a floor the load screen meets grades the wrong screen.

MEASURED (GTX 570, executor=1 + readback=1, runs fh1-free-roam-20261023T163504Z and
roam-late-20261023T163000Z)
- Load screen (2400/3000): mean 88.897/88.897, 250 colours, MAE 0.000 - the old scenario's blue.
- World (game_mode 17 at every capture): 3600 mean 81.8 / 11,580 colours; 4200 mean 44.5;
  5400 mean 88.2; min 0 max 255. HUD is correct and legible: PRESS (A) TO ACCELERATE, the
  speedometer ring 0-8, SAVING, 2.6 MI, the song toast "Everyday (Netsky VIP Mix)", street
  name FLYNT.
- MAE(3600,4800) = 38.844, MAE(4200,5400) = 39.031, so the new assertions pass; the old
  load-screen captures measure MAE 3.629 (1200 vs 1800) and 7.902 (1200 vs 1500), i.e. below
  the 10 floor - the gate would have caught the old timeline, it is not vacuous.
- Cost of the world: 9.2-10.0 FPS median (108,600-115,266 us), simulation tick 18.11 Hz,
  simulation-time wall ratio 1.0013 (1 invalid delta), 914 presents with 0 duplicates.

WHY THE WORLD COMPOSES WRONG (recorded, not fixed): blue sky is 88.2% (3600), 45.7% (4200),
95.6% (5400) of the frame; black 5.1/51.1/1.1%; everything else (HUD + world fragments) 3.2-6.7%.
The fragments are strips in the top-left and top-right corners with a black vertical band beside
them (see .local/diag/free-roam-roam-3600.png). Renderer-side facts from the traced run
fh1-free-roam-20261023T162024Z: the scene draws into 0/32/4x/c3 (1280x256, three chunks) and
0/32/4x/c12 (ownership-redirected from c3), the guest resolves those per chunk to guest memory,
and the presented front buffer comes from 0/16/1x/c10 - a key that was NEVER a draw target
(rt_bound=0 on all 227 of its resolves, 34 surfaces in the registry). So the composited frame
is not the surface the world was drawn into: 2D/HUD passes land correctly, the 3D chain does not.
That is the next defect, not a scenario one.

## PHASE 6 — THE UPLOAD GATE GRADES A RATE, NOT A RUN TOTAL (2026-10-23, this pass)

VERDICT: d3d11-texture-uploads no longer fails a correct but slower frame path for doing less
total work inside the scenario's fixed 70 s. The gate now measures uploads per presented frame.
With that floor, the four gates with executor=1 + readback=1 pass 4/4 (they were 3/4 on the old
12,000 total), and the default run still passes.

WHAT WAS WRITTEN (.local/check_d3d11_gate.py)
- MIN_UPLOAD_TALLY 12,000 is replaced by MIN_UPLOADS_PER_FRAME 4.0, with MIN_UPLOAD_TALLY 500
  kept only as a near-empty-run guard. The denominator is the last `D3D11 draws frame=N` in the
  runtime log - the scenario's frame count - so a run that reaches fewer frames in its 70 s is
  graded on the work it should still do per frame. The failure message reports uploads, frames
  and the rate; the upload-path failure patterns and every other gate are unchanged.

CALIBRATION (8 archived d3d11-texture-uploads runs: 9,500-16,500 uploads over 1,968-3,425
frames): 4.58-4.83 uploads per frame, including 4.68-4.69 on the executor+readback path the old
12,000 total rejected at 10,000. The 4.0 floor sits ~13% under the slowest observed mix.

VERIFIED
- Default run: 16,500 uploads / 3,421 frames = 4.82 per frame, PASS.
- Executor=1 + readback=1: 10,000 / 2,137 = 4.68 per frame, PASS (was FAIL "upload tally 10000
  below 12000").
- Negatives (synthetic run dirs through the same checker): 300 uploads over 2,000 frames
  (0.15/frame) FAILs on the rate; a log with no upload tally FAILs; a good rate with a
  CreateTexture2D failure FAILs on the upload-path pattern.
- Full suite, executor=1 + readback=1, final binary: 4/4 PASS - cleared-frames 30.67 Hz /
  32,766 us / 105,311 draws, texture-uploads 4.68/frame, native-resolve-path 12,924 resolves
  0 skipped, draws 106,521 draws.

STILL OPEN: notice-frames' p0360 is still a uniform black frame (PHASE 5); the cvar defaults,
the C2 fill cost and scaled targets are open as before.

## PHASE 5 — THE GATE HARNESS ACCEPTS DARK FRAMES (2026-10-23, this pass)

VERDICT: a capture is no longer rejected for being dark. `ppm_summary` used to reject any image
with `mean < 1` of 255; it now rejects uniform output by the byte span (unchanged) and, for a
mean under 1, requires actual structure: a dominant background colour, at least 0.1% of the
frame differing from it by >= 8 in a channel, and either >= 4 content shades or >= 1% solid
content. With that, the readback-on pipeline runs the battery: trace-short passes (it used to
abort at t0300), and the four gates with executor=1 + readback=1 run to completion, 3 of 4
passing - the remaining failure is texture-uploads' work-done floor, not a capture.

WHY: the rejected capture was the title's own loading screen (mean 0.83, 0.42% content, 239
shades) at frame 330, and its black HUD frame (mean 0.467, 2.82% content, 1,114 shades). Both
are real frames; the old rule conflated "dark" with "blank". A truly blank capture is uniform,
which the span rule already rejects, so this does not let a failed renderer through.

WHAT WAS WRITTEN
- tools/run-fh1-render-test.py: has_visible_content (dominant background, content share and
  shade count) and ppm_summary's low-mean branch. The error text is unchanged, so existing log
  checks still match. The `expect_cleared` branch still requires exact uniformity.
- tools/tests/test_run_fh1_render_test.py: PpmSummaryTests - a synthetic loading screen at
  1280x720 is accepted (mean < 1), while all-black, a single stray pixel and a 40x40 solid patch
  are rejected. Whole file: 64 tests, OK.

MEASURED
- trace-short with executor=1 + readback=1: PASS, captures mean 0.829 and 0.467 (both formerly
  rejected), 377 samples, median 33.16 ms.
- Four gates with executor=1 + readback=1 (.local/rbsuite_*):
    cleared-frames       PASS  30.97 Hz, median 32,769 us, 104,793 draws (Gate C: FAIL 29.32 Hz /
                                35,144 us) - now above the 30 Hz cadence floor
    native-resolve-path  PASS  263,916 draws, 12,944 resolves, 0 skipped
    draws                PASS  106,338 draws, title frame
    texture-uploads      FAIL  upload tally 10,000 below 12,000 (work-done floor, unchanged
                                from Gate C; captures accepted, runner exit 0)
- Four gates with defaults (executor 0, readback off): 4/4 PASS - cleared-frames 49.10 Hz /
  20,020 us, texture-uploads 16,500 uploads, native-resolve-path 34,809 resolves 0 skipped,
  draws pass.

STILL OPEN after this phase: notice-frames produces all 28 captures but is rejected at p0360,
which is a genuinely uniform black frame (span 0, one colour, mean 0.0) captured during an
input-tap transition - the span rule doing its job, not a dark real frame. Accepting it would
also accept a black renderer. The texture-uploads work floor at the time was open; it is CLOSED
by PHASE 6 at the top of this file. The readback default stays false while the p0360 probe and
the executor's defaults question are open.

## PHASE 4 — THE FRONTBUFFER READBACK LANDS THE PRESENTED EXTENT (2026-10-23, this pass)

VERDICT: fh1_d3d11_resolve_readback now lands exactly the frame the swap presents - the guest
range texture fetch constant 0 describes - once per swap, after the presentation wait and before
the untiling upload, instead of copying every resolve's destination extent back as it was
produced. The old per-resolve form was not writing a wrong extent (the content it lands is the
resolve's own; see below), but it could not guarantee the presented buffer: the last resolve of
a frame can cover only part of it, and it stalled the GPU once per resolve (13,682 resolve calls
by swap 600 of notice-noinput - ~23 per swap). The swap-time copy is one readback of 3.6 MB per
swap and is measurably cheaper. Readback stays OFF by default: with it on the presentation is truthful, and
that still exposes the C2 fill cost and the executor's defaults question rather than hiding it.

WHAT THE EARLIER VERDICT GOT WRONG, MEASURED: Gate C's "the readback blanks the front buffer"
was the runner's degeneracy rule, not the readback. The rule is `mean < 1` over all bytes
(tools/run-fh1-render-test.py:577). At frame 330 with the readback on, the game presents its
"LOADING... PLEASE WAIT" screen: mean 0.83, 0.4% non-black samples, max 252 - real white text
on black, rejected as "blank or degenerate output". The content is real and NOT the readback failing:
notice-noinput with the readback on presents the "PRESS START" menu (mean 4.706, byte-identical
across all 600/1200/1800/2400 captures and accepted by the runner), and the d3d11-draws gate
with the readback on captures the same menu and passes. The pipe from resolves to presentation
works; the captures with the readback on are the game's own frames instead of the CPU's frozen
boot frame. What the rule calls degenerate is a dark frame the title itself is showing at that
instant.

WHY THE OLD FORM WAS THE WRONG PLACE, NOT THE WRONG DATA: every per-resolve landing was checked
against the staging buffer it came from (log `FH1 resolve staging` / `FH1 resolve readback
landed`): first words equal, e.g. staging call=2 info=000E0050 first=005600FF -> readback at
1D728000 guest_first=005600FF. But it was one WaitForGpu + mirror read per resolve - 23 per swap
in notice-noinput - and it copied resolve destinations, not the presented buffer. (The old form
cost ~6.5 FPS on the readback path; the swap-time form is measured below.)

WHAT WAS WRITTEN
- src/graphics/d3d11/texture_cache.{h,cpp}: GetSwapGuestExtent - the swap fetch's guest base
  (base_page << 12) and length (the base level's guest byte size, GetGuestBaseSize). It uses the
  same BindingInfoFromFetchConstant key RequestSwapTexture uses, so the readback and the upload
  can never name different ranges.
- src/graphics/d3d11/command_processor.cpp: IssueSwap, after the existing WaitForGpu and before
  RequestSwapTexture, copies that extent from the GPU mirror into guest memory (ReadbackFromHostBuffer
  + SharedMemory::CopyReadbackToGuest) when fh1_d3d11_resolve_readback is on, tallies it, and
  (fh1_swap_fetch_trace) logs readback_first/guest_first plus non-zero word counts from both
  sides, with pages_valid/gpu_written. The per-resolve guest copy is gone; the staging-to-mirror
  copy and MarkRangeAsResolved are unchanged. A warning is logged when the readback is on with
  the executor off, because that combination faithfully presents empty surfaces.
- flags.cpp help text now describes the swap-time behaviour; dead swap_front_base_ removed.

VERIFIED (GTX 570, hidden, .local/preview)
- FH1 frontbuffer readback lines: `range=1BDB1000+3768320 fetch=1280x720 pages_valid=920
  gpu_written=920 readback_first=C0000000 guest_first=C0000000 readback_words_nz=28800
  guest_words_nz=28800` - the extent, the readback's first word and guest memory's first word
  agree, and the counts agree. At swap 0 the range's pages are not GPU-written yet and nothing
  is copied (pages_valid=0), which is the correct no-op.
- d3d11-draws gate with executor=1 + readback=1: PASS, median 32.61 ms / 30.66 FPS (Gate C's
  executor=1 readback=off was 35.10 ms), capture mean 4.706 = the "PRESS START" menu. Gate C
  rejected this combination as blank; it is now the gate's passing capture.
- notice-noinput with executor=1 + readback=1: pass, four captures mean 4.706, 948 swaps each
  with one readback, against 13,682 resolve calls by swap 600 (~23 per swap); median 42.05 ms
  against 49.13 ms for the per-resolve readback on the same scenario (948 vs 854 samples, same
  41.7 s window) - 14.4% faster, from one GPU sync per swap instead of ~23.
- readback=1 with executor=0: runner rejects (empty presentation) and the log names why: the
  surfaces the resolve reads are never filled without the executor. Truthful, not a defect.
- Default path (readback off): four-gate suite GATES_PASSED=4 - cleared-frames 44.21 Hz /
  152,187 draws, texture-uploads 14,500 uploads, native-resolve-path 0 skipped, draws pass;
  d3d11-draws and d3d11-native-resolve-path re-run on the final binary also pass (37,463
  resolves, 0 skipped), and the d3d11-draws capture stays md5 b8f68e89c619.

STILL OPEN after this phase: the harness threshold that rejected the loading screen - CLOSED by
PHASE 5 at the top of this file (structure-based blank detection; trace-short and the gate
battery now run with the readback on). The readback default stays false (texture-uploads' work
floor and scaled targets are open), and the C2 fill cost stays open as before.

## PHASE 3 — GATE C: THE EXECUTOR-ON BATTERY, AND BOTH DEFAULTS STAY (2026-10-23, this pass)

VERDICT: with fh1_native_executor=1 the picture is byte-identical and stable on every scenario
that finishes, but the executor-on battery FAILS TWO OF THE FOUR GATES on their pacing and
volume floors, because the executor adds ~10.6 ms to the frame (24.5 -> 35.1 ms median at this
session's basis). Parity and stability are not the problem: all 6 gate captures plus trace-short
x2 and notice-noinput x4 are md5 b8f68e89c619, every gate run has 0 device removals, skipped
draws 0 and transfer_skipped 0/0/0 over 20+ statistic lines. So the DEFAULT DECISION is: both
defaults stay where they are - fh1_native_executor 0 (off) and fh1_d3d11_resolve_readback false -
and the criteria that would flip each are written below with the numbers that would have to move.

WHAT WAS RUN (executor=1 unless said otherwise, GTX 570, hidden, .local/preview)
- Four gates through .local/muse_gates_all.ps1 with REX_FH1_NATIVE_EXECUTOR=1:
    gate                     result  median    present  draws    (executor=0, same session)
    d3d11-cleared-frames     FAIL    35.14 ms  29.32 Hz  95,443  24.52 ms / 39.99 Hz / 138,949
    d3d11-texture-uploads    FAIL    35.16 ms  28.09 Hz  97,257  24.46 ms / 40.39 Hz / 141,305
    d3d11-native-resolve-path PASS   35.78 ms  19.14 Hz 239,500  24.97 ms / 32.38 Hz / 422,091
    d3d11-draws              PASS    35.10 ms  28.94 Hz  97,291  24.77 ms / 39.53 Hz / 138,143
  The two failures are the gate conditions, not capture regressions:
    cleared-frames: "present cadence 29.32 Hz below 30.0" (median 35.1 ms is inside the
      40 ms cap; the cadence floor is what the executor's +10.6 ms breaks).
    texture-uploads: "upload tally 9500 below 12000" - the tally is a work-done floor and the
      executor's slowdown cuts the run's draw volume 138,949 -> 97,257 (-30%), the same ratio
      as the frame time (24.77/35.10 = 0.71 vs 97,257/138,949 = 0.70).
- Executor counters in those four runs: prepares == binds == draws, skipped=0, claims and tiles
  bounded (surfaces 13-16), transfer_skipped=0/0/0, 0 device removals, words 18,584 dispatches
  for 376,704 rectangles in cleared-frames and 19,121 for 390,440 in draws (~20 rectangles per
  dispatch); native-resolve-path shows the union-waste fallback in use (8,099 batches for 8,761
  dispatches, so 662 dispatches ran one rectangle at a time).
- Content probes: trace-short x2 identical (b8f68e89c619), notice-noinput x4 identical. The
  notice-frames time series (28 captures) could not be run to completion with the executor on
  in this session: it reached frame 2,158 when the harness's own 263 s budget expired, against
  the executor=0 run that finished 28 captures at frame 2,502 in 218 s. That is the session's
  speed (the same scenario took 24 s earlier in the day) plus the executor's +43% frame cost,
  not a hang: the run was still drawing, 0 device removals, no skipped draws.
- Readback trial (2x2 on trace-short, the second defaults decision):
    executor=0 readback=off  pass, captures b8f68e89c619
    executor=1 readback=off  pass, captures b8f68e89c619
    executor=0 readback=on   runner rejects: "capture t0300.ppm: blank or degenerate output"
    executor=1 readback=on   the same rejection
  So the readback still replaces the presented frame with an empty extent, with or without the
  executor; the executor is not what makes it blank, and the flag remains unusable as a default.

DEFAULTS
- fh1_native_executor stays 0. Criteria to flip it: the four gates must pass with it on, which
  needs its added frame time to fall from ~10.6 ms to <= ~8 ms (median <= 33 ms keeps the
  cleared-frames cadence above 30 Hz and keeps the upload tally above 12,000). The measured
  components of that cost are the C2 colour-transfer fill (claims-as-bounds, the C4 notes' 15%)
  and the depth passes (8 stencil passes per transfer, 5.5 ms at C3's basis). Parsing and binding
  are not part of it: prepares == binds == draws and the counters are clean in all four runs.
- fh1_d3d11_resolve_readback stays false. Criteria to flip it: with it on, the presented captures
  must equal the readback-off captures and guest_first reading must show the drawn frame. Today
  it blanks the frame in both executor states, so it is a correctness blocker before it is a
  performance question.

STILL OPEN after Gate C: the readback's write target - CLOSED by PHASE 4 at the top of this
file, which lands the presented fetch extent once per swap - plus the C2 fill cost, per-owner
rectangle splitting, scaled targets, and the parity report (it waits on its corpus pack).

## PHASE 2 — C4: THE WORDS PASS IS BATCHED PER DESTINATION/SOURCE PAIR (2026-10-22, this pass)

VERDICT: the depth transfer's compute dispatches are batched. A claim's runs are queued, and
flushed as one transfer per (previous owner, stencil) pair, so every rectangle of one
destination/source pair shares ONE compute dispatch (fh1_native_transfer_words_batch.cs.hlsl,
whose dispatch covers the rectangles' bounding box) and one set of depth and stencil passes
instead of one transfer and one dispatch per run. Dispatches per frame in d3d11-draws fall from
198.5 to 9.9 (20.1x fewer) carrying the same rectangles and the same words (10.32M -> 10.42M words
per frame, +0.9%), and the depth-transfer cost above executor=0 falls from +35.5 ms to +11.7 ms
over the same 70 s scenario. Output stays byte-identical in every mode, the depth-verify probe
still reports content moving into the claimant, the four-gate suite passes. The dispatch-free
"direct from-source" fallback was implemented and measured too: it is NOT faster (86.1 ms vs
63.4 batched) because it recomputes the previous owner's word in every one of the nine passes,
so the batched words path is what runs and the direct path stays behind a bisect switch.

WHAT WAS WRITTEN
- Shaders: fh1_native_transfer_words_batch.cs.hlsl (new; FH1_WORD_TYPED, a rect table of up to 32
  rectangles after the eight header words, the dispatch covering their bounding box and each
  thread keeping the rectangle it falls in). compile-fh1-native.sh builds its six variants
  (color/depth/uint x single/MSAA) at cs_5_0 into bytecode/d3d11_5_0 only - the Direct3D 12
  executor still passes one rectangle per dispatch through fh1_native_transfer_words.cs.hlsl, so
  the d3d12_5_1 and vulkan_spirv sets are untouched.
- src/graphics/d3d11/fh1_native_executor.cpp and its header: ClaimTargetTiles now queues
  QueueTransferRun's chunks (rectangles + previous owner + the stencil flag read at claim time)
  and FlushClaimTransfers issues one TransferRects per (previous owner, stencil) pair, up to 32
  rectangles per call; TransferDepthRects issues one batched dispatch for the pair when the
  bounding box is at most 4x the rectangles' own area, and the single-rectangle shader per
  rectangle otherwise (a batch of 1 too). The transfer constants buffer grows to 288 bytes (eight
  header words + 64 for the rect table). New counters: transfer_batches_, transfer_word_rects_,
  transfer_words_ (words written), transfer_depth_direct_.
- Direct path (the prescribed fallback, fh1_debug_transfer_direct): the existing
  fh1_native_transfer.ps.hlsl FH1_DEST_KIND 1/2 d3d11 headers (already built by the same script)
  run the depth and stencil passes straight from the source, with no words buffer, no UAV and no
  compute dispatch. TransferDirectSupported keeps a 16_16[_16_16] colour predecessor on the words
  path unless the host keeps the full snorm16 range: the direct shader leaves fh1_fixed16_scale at
  its 32x default while the words pass sets it from the fixed16 flag.
- Bisect switches: fh1_debug_transfer_unbatched (one transfer and one dispatch per run, the C3
  behaviour), fh1_debug_transfer_direct (the direct path), fh1_debug_transfer_off (unchanged).
- Probe: depth transfers of the same pixel rectangle are still compared (rects[0] of each call),
  but a probe whose predecessor has another layout is labelled "coarse: the predecessor's
  rectangle covers other EDRAM samples" instead of "MISMATCH" (the two rectangles then cover
  different EDRAM sample sets and their empty counts are not comparable), and a colour
  predecessor is only probed at the same layout and sample count. Depth copies through
  CopySubresourceRegion are probed too, where the comparison is exact by construction.

DETERMINISTIC VERIFICATION (trace-short-20261023T..., d3d11-draws-20261023T..., GTX 570)
  d3d11-draws, executor=1, fh1_swap_fetch_trace:
    depth transfer verify 720/16/1x/d1 -> 1024/32/4x/d1 rect=0,0-1280,8 texels=10240
      empty_texels dest_before=10240 dest_after=9088 source=9088
      (the claimant's depth now holds its predecessor's words)  x4 lines in one session
    depth_verified=4, transfer_skipped=0/0/0, skipped draws 0.
  trace-short, executor=1: depth_verified=8, transfer_skipped 0/0/0 - the same rectangle
  (720/16/1x/d1 -> 720/16/4x/d1, dest_before=4944, dest_after=4544, source=3968) reported as
  coarse, because a 1x predecessor's rectangle covers the same pixels but other EDRAM samples;
  the two colour-predecessor probes ahead of them inherit an all-empty colour surface and are
  logged as such, not counted.
  Counters (d3d11-draws, frame 1100, batched): words=10846 word_rects=222989
  words_written=11,458,396,160 batches=10846 depth_passes=10846 stencil_passes=86816
  depth_verified=0 (probe off in the cost runs), skipped draws 0.
  Unbatched, same scenario: words=158835 == word_rects=158835 == depth_passes=158835 (one
  rectangle per dispatch). So per frame: 9.9 dispatches carrying 202.7 rectangles and 10.42M
  words, against 198.5 dispatches carrying 198.5 rectangles and 10.32M words - same work, 20.1x
  fewer dispatches.
  Two runs of every mode: identical counters up to the run length and identical captures.

GATES
- Captures md5 b8f68e89c6192577747ab452aa5598ef in every configuration: executor=0, executor=1
  batched words, executor=1 unbatched words, executor=1 direct - trace-short (2 captures),
  d3d11-draws (1), notice-noinput (4, executor 0 and 1).
- Four-gate suite (.local/muse_gates_all.ps1, executor default off): GATES_PASSED=4,
  cleared-frames 40.79 FPS derived, texture-uploads 40.88, native-resolve-path 40.06 (988
  frames), draws 40.37 - every RUNNER_EXIT=0 / GATE_EXIT=0.

COST, MEASURED (d3d11-draws, 70 s and trace-short, GTX 570, hidden, .local/preview, 2 runs each;
median host frame time after a 5 s warmup, the C1-C3 protocol)
  d3d11-draws            run 1    run 2     delta vs executor=0
  executor=0             51.69    51.65     -          (19.35 FPS)
  executor=1, batched    63.43    63.44     +11.7 ms   (15.76 FPS)
  executor=1, unbatched  87.22    87.21     +35.5 ms   (11.47 FPS)
  executor=1, direct     86.14    86.18     +34.4 ms   (11.61 FPS)
  trace-short: 52.01/52.06, 64.01/63.41, 87.88/87.74, 86.79/86.62 - the same order and the
  same deltas. Spread inside a configuration is under 0.2%.
  BASIS: this session's executor=0 median is 51.7 ms against C3's 24.5 ms on the same protocol
  and binary path (the host is ~2.1x slower this session - no other pinyon_shift instance was
  running, checked), so the absolute FPS lines are not comparable to C3's table; the deltas and
  the dispatch counts are.
  WHERE THE REMAINING 11.7 ms GOES: the batched run makes 9.9 transfer calls and 9.9 dispatches
  per frame where the unbatched one makes 198.5 of each, for the same 202 rectangles, the same
  10.4M words and the same 8 stencil passes per transfer - so the 23.8 ms the batching removes is
  the per-call overhead (state changes, constant uploads, UAV bind/unbind and the dispatch), about
  126 us per call at this session's basis, which is C3's measured 146 us. What is left is the
  passes themselves: 8 stencil passes per transfer, measured at 5.5 ms in C3 on a session 2.1x
  faster, i.e. ~11.6 ms here - the 11.7 ms observed. The direct path confirms the attribution from
  the other side: it removes the dispatch entirely (0 per frame) yet costs the same as unbatched
  (86.1 vs 87.2 ms), because it does the source word computation in all nine passes instead of
  once per sample in the dispatch (+23 ms of fill/ALU against the batched words path).

STILL OPEN (unchanged by this pass): the claims-as-bounds fill cost of the C2 colour transfers, per-
owner rectangle splitting (SplitByOwner), the resolve readback default, scaled targets. The
batched words pass is the depth-transfer default; the direct path stays a bisect. Gate C makes the
executor's defaults call.

## PHASE 2 — C3: DEPTH DESTINATIONS THROUGH THE EDRAM WORDS PASS (2026-10-22, this pass)

VERDICT: a depth destination whose predecessor is another surface is no longer counted and
skipped (`transfer_skipped_format`): it goes through the EDRAM words pass - a compute dispatch
that produces the previous owner's guest word for every destination sample, then the from_words
depth pass (which also resets stencil) and one pass per stencil bit. The words are bound through
TYPED R32_UINT views, one element per guest EDRAM word (Option A): no raw view is created on this
backend at all, because feature level 11_0 / Fermi 391.35 stores raw UAVs reliably only at low
offsets and refuses a sub-range raw view outright. Executor=1 output stays byte-identical to
executor=0, the four-gate suite passes (4/4), and no device removal occurs.

WHAT WAS WRITTEN
- Shaders (sources + SM 5.0 bytecode): FH1_WORD_TYPED variants of fh1_native_transfer_words.cs.hlsl
  and fh1_native_transfer_from_words.ps.hlsl (RWBuffer<uint> / Buffer<uint>, one word per
  element), and fh1_native_depth_probe.ps.hlsl (single-sampled and FH1_PROBE_MSAA). Six words
  variants (color/depth/uint x ms), four from_words variants (depth/stencil x dms).
- compile-fh1-native.sh: the third-pass block that builds exactly these headers at cs_5_0 / ps_5_0;
  the d3d12_5_1 and vulkan_spirv sets are untouched.
- src/graphics/d3d11/fh1_native_executor.cpp: TransferDepthRects (words dispatch per rectangle ->
  depth pass -> 8 stencil passes with StencilWriteMask 1<<bit, REPLACE at 1<<bit), the growable
  typed words buffer (1 MB doubling to 64 MB, SRV+UAV R32_UINT, logged with HRESULT and
  GetDeviceRemovedReason), depth/stencil pipeline states, per-transfer sample mask 0b1001 for
  2x-as-4x, and the transfer constants buffer grown to 32 bytes (eight words for the words pass).
- Probe (fh1_swap_fetch_trace only): CountEmptyDepth reads a depth rectangle through a shader
  resource view into an R32_FLOAT target and back through staging (Direct3D 11 cannot stage-copy
  a depth-stencil resource); VerifyDepthTransfer compares the claimant's empty texels with its
  predecessor's, counting only transfers that actually inherit content (8 of them, 60 attempts).

DETERMINISTIC VERIFICATION (trace-short-20261023T041927Z / T041942Z and the final binary's
T042843Z, GTX 570, 4x depth)
    depth transfer verify 720/16/1x/d1 -> 720/16/4x/d1 rect=0,0-640,8 texels=5120
      empty_texels dest_before=400 dest_after=0 source=0  (claimant's depth now holds its
      predecessor's words)  - and the same for the next seven rectangles
  dest_before 400 -> dest_after 0 is content moving in: the predecessor's rectangle had no empty
  texel and the claimant, which had 400, has none after the transfer. The two colour-predecessor
  probes that precede them (0/16/4x/c2 -> 720/16/4x/d1) inherit an all-empty colour surface and
  are logged as such, not counted.
  Counters (frame 100): words == depth_passes == 9,235 / 8,646, stencil_passes == 8 x that,
  transfer_skipped 0/0/0, depth_verified 8, skipped draws 0. Two consecutive runs: identical
  numbers and identical captures.

GATES
- Captures: executor=1 md5 b8f68e89c6192577747ab452aa5598ef on both captures, equal to the
  executor=0 run of the same build and to the pre-C3 binary.
- Four-gate suite (.local/muse_gates_all.ps1, executor default off): GATES_PASSED=4,
  cleared-frames 40.78 FPS / 138,405 draws, texture-uploads pass, native-resolve-path 434,171
  draws, draws 40.91 FPS / 140,876 draws. (The suite runs the executor at its default, off; the
  executor's own A/B is below.)

COST, MEASURED (d3d11-draws, 70 s, GTX 570; same build, executor on/off)
  executor=0                      40.75 FPS  median 24.5 ms
  executor=1, C3 full             16.06 FPS  median 62.3 ms
  executor=1, stencil passes off  17.63 FPS  median 56.7 ms  (+5.5 ms for 8 stencil passes)
  executor=1, words dispatch off  34.01 FPS  median 29.4 ms  (dispatch loop is +27.3 ms)
  So the pass cost is affordable on this host; the per-transfer compute DISPATCH is not
  (~146 us each, 185 depth transfers per frame in this scenario). That is exactly the
  transfer-cost cut already queued as the next item: batch the words of one destination/source
  pair into one dispatch (per-claim batching included) so the dispatches drop by an order of
  magnitude, and only then consider the direct from-source depth path as a dispatch-free fallback.
  Nothing here changes the executor's default (off), so no gate or scenario regresses.

STILL OPEN (unchanged by this pass): per-owner rectangle splitting (SplitByOwner), the resolve
readback default, scaled targets, and the transfer-cost cut above.

## PHASE 2 — C2: A CLAIMANT SURFACE RECEIVES THE TILES IT CLAIMS (2026-10-22, this pass)

VERDICT: transfers are implemented, built (BUILD_EXIT=0) and measured: a surface that claims
EDRAM tiles another surface wrote now receives their content before its draw runs, through
CopySubresourceRegion when the host format and geometry are identical and through the ownership
transfer pixel shaders (the D3D12 set, compiled for SM 5.0) when they are not. Executor=1 output
stays byte-identical to executor=0 on every scenario, and the sustained draw path is stable.

WHAT WAS WRITTEN
- Files: src/graphics/d3d11/fh1_native_executor.cpp and its header. Claims now follow the
  draw's rectangle (viewport intersected with scissor,
  passed from command_processor.cpp) instead of the whole surface height, so claims and the
  transfers they trigger are proportional to the draw.
- Claim path: Fh1EdramTiles::ClaimRect for one previous owner, per-row Claim when the rectangle
  mixes owners; every run it returns is transferred (TransferRun -> TransferRects).
- Transfer draws: the shader is picked by destination kind (color RTV, or UINT RTV for formats
  with 16/32-bit float channels), destination MSAA, and source kind (color, depth, raw channel
  bits) x source MSAA - the same FH1_SOURCE_*/FH1_DEST_KIND variants the D3D12 executor ships.
  Constants are the two packed EDRAM layouts plus the flags word (gamma unorm16, fixed16 full
  range, scale), set through a 16-byte PS constant buffer; the fullscreen triangle comes from
  fullscreen_cw_vs.h, the viewport and scissor are the tile rectangle, and the destination's
  render target is bound with no DSV and depth/stencil disabled.
- fh1_debug_transfer_off (bool, GPU): claims are still recorded and counted, no transfer is
  performed - the bisect switch used for the A/B below.

DETERMINISTIC VERIFICATION (fh1_swap_fetch_trace; trace-short-20261023T..., GTX 570)
  The first 8 single-sample transfers of a session read the claimant's rectangle before and
  after the transfer and the predecessor's rectangle, counting EMPTY texels (all bytes zero,
  format-width aware - byte counts would call a dark 16-bit value "content" and a packed word
  "empty").
    c3 -> c1  rect 0,0-1280,720  texels 921600  empty: dest_before 0, dest_after 0, source 0
    c1 -> c2  rect 0,0-1280,368  texels 471040  empty: dest_before 0, dest_after 471040,
                                                   source 471040
  The first line is the mechanism working: a claimant that held no empty texel still holds none
  after inheriting, matching its predecessor exactly. The second is the mechanism's known limit
  (below), reported rather than hidden: the predecessor's rectangle was entirely empty and the
  transfer wrote that emptiness over a populated claimant, because the claim says the tiles the
  draw *may* write, not the tiles it wrote.
  Zero-texel counts are comparable across formats; the earlier byte-count version of this probe
  was misleading and was replaced (8_8_8_8_GAMMA is hosted as 16-bit channels, 2_10_10_10 as a
  packed word, so the two sides differ in bytes per texel).

COST, MEASURED (GTX 570, hidden window, .local/preview, same build, 2 runs each)
- trace-short: executor=1 with transfers 41.65 / 41.70 FPS; executor=1 with
  fh1_debug_transfer_off 48.56 / 49.03; executor=0 49.23 / 48.87. So the executor's claims,
  ownership and binding cost nothing measurable, and the transfers cost ~15% here.
- d3d11-draws (2870 frames, 143,633-167,937 draws): executor=1 with transfers 41.54 FPS,
  executor=0 48.78. Same ~15%, and the transfer work is ~10,000 tiles/frame (~10 full-screen
  equivalents), which is what the 15% buys: the same EDRAM tiles are re-encoded into a different
  host format whenever the guest alternates surfaces over the same ranges.

GATES
- Captures are md5 b8f68e89c619 in every run of every mode (trace-short x4, d3d11-draws x2,
  notice-noinput x4): executor=1 with transfers, executor=1 with transfers off, and executor=0
  all present the same frame.
- Sustained d3d11-draws with transfers: pass, 2867 measured frames, 0 device removals,
  prepares == binds == draws with skipped=0 through all 28 statistics lines, surfaces 13-16.
- notice-noinput (the Phase 1.7 (iii) scenario that used to be blank) still passes with
  transfers on, all four captures non-black.

STILL OPEN (named, with why)
- Depth destinations: CLOSED by C3 above (the EDRAM words pass, typed R32_UINT views).
- Claims are bounds, not writes. An un-drawn part of a claiming surface owns tiles all the same,
  so its emptiness can be transferred over a populated claimant (the c1 -> c2 line above). The
  D3D12 executor shares this design and bounds the vertical extent with DrawExtentEstimator;
  adopting that estimator, or per-tile written tracking, is the fix. Nothing user-visible follows
  from it today: every capture is identical with transfers on and off.
- Per-owner rectangle splitting (SplitByOwner): a mixed-owner copy still falls back to the copy's
  own key.
- Depth tail: CLOSED by C3 above. Scaled targets (`Initialize` refuses scale != 1) and the
  readback default (still false) remain.

## PHASE 2 — C1: THE D3D11 FH1 NATIVE EXECUTOR IS IN, AND DRAWS FILL THE SURFACES THE RESOLVE READS (2026-10-22, this pass)

VERDICT: C1 is implemented, built (BUILD_EXIT=0) and measured. Each draw derives its EDRAM
surfaces from the guest registers, claims their tiles, binds them through
D3D11Fh1NativeExecutor, and the resolve now reads the surface the draws filled - including
when the copy names a different format than the draws used. Executor=1 output is byte-identical
to executor=0 on every scenario run (the off path is untouched), and the formerly-blank
Phase 1.7 (iii) scenario now captures a real frame.

WHAT WAS WRITTEN
- include/rex/graphics/d3d11/fh1_native_executor.h (new): D3D11Fh1NativeExecutor over the
  shared surface registry (D3D11Fh1NativeSurfaces), the shared key geometry
  (fh1_edram_surfaces.h) and the shared tile ownership (Fh1EdramTiles).
- src/graphics/d3d11/fh1_native_executor.cpp (new): PrepareTargets (derive depth + color keys,
  drop a color slot that shares a base, Claim(transfer=true) over the surface height,
  MarkStencil), BindTargets (create/find the registry surfaces, OMSetRenderTargets, report the
  bound bits and formats the pipeline key needs), ResolveSourceOwner, accounting.
- fh1_native_executor cvar (int, GPU, kRequiresRestart): 0 = off (default), 1 = on,
  2 = reserved and logged as running off. Off compiles to the previous render target path
  unchanged (native_executor_ stays null).
- Wiring in src/graphics/d3d11/command_processor.cpp: construction after native_surfaces_
  (Initialize refuses scale != 1 and no registry, logging why), shutdown, BindDrawRenderTargets
  (PrepareTargets + BindTargets instead of the path's own key derivation and binds),
  NativeDrawIssued after the draw, OnSwap at IssueSwap, LogStats every 100 frames and at
  shutdown.

ONE DERIVATION, MEASURED: pitch comes from Fh1PitchTiles and height from Fh1SurfaceHeight, the
same functions the resolve plan uses. The executor's first version rounded the pitch itself and
bound surfaces keyed differently from the ones the resolve read - the draw keys were not among
the resolve's keys (rt_bound=0) and the resolve read a fresh zero surface. With the shared
derivation the resolve's own keys are draw-bound (`rt_bound=1`) and hold the frame.

EVIDENCE (GTX 570, window hidden, state root .local/preview; trace-short, trace on)
- Executor accounting, trace-short-20261023T... (p2_owner2): frame=200 prepares=7129
  binds=7129 draws=7129 skipped=0 claims=8323 owner_resolves=366 surfaces=13. prepares always
  equals binds and draws, skipped=0 in every sample of every run.
- The resolve writes what its source surface holds: call=0 key=0/16/1x/c2, source surface
  1280x2048 with 8240 of 8240 sampled texels nonzero inside the 1280x720 rect and 0 below it
  (a drawn frame, not a cleared surface), first C6F16C6F; the staging result is
  6F6CF1C6, first byte-swapped word of that source, 921,600 words, zero=20480.
- OWNERSHIP SOURCE SELECTION, the piece that makes "the surface the resolve reads" true when
  the formats disagree. The tile claims record the writing surface; IssueCopy now asks
  ResolveSourceOwner for the single owner of the tiles under the copy's rectangle (same sample
  mode and depth-ness required - a 4x surface is not a 1x surface's words) and reads that
  surface, letting the shader decode through the copy's format with the owner's layout (its
  two layout words). Before/after on the same scenario:
  * `0/32/4x/c12` (the 4x scene copy): 528 resolves in 700 frames, rt_bound=0, source surface
    nonzero texels 0 -> its staging output was all zeros. After: source `0/32/4x/c3`, the
    surface the draws wrote (1280x512); 192 staging resolutions all content-bearing,
    words_nz=327680 of 327680, zero=0, first 42082144.
  * `0/16/1x/c0` (the front-buffer copy, guest 8_8_8_8): source surface empty before, output
    zeros. After: source `0/16/1x/c1` (the 8_8_8_8_GAMMA surface the draws wrote),
    words_nz=921600, first 005600FF.
  * Still empty, named and not hidden: 65 of the `0/16/1x/c0` resolves and 2 of the
    `0/16/2x/c12` resolves (the rectangle mixes owners, or no owner in the same sample mode) -
    these fall back to the copy's own key, and C2's per-owner runs are what fixes them.
  * transfers_due grows with the claims (70,043,648 tile-runs by frame 2800 of the sustained
    run): that is C2's workload recorded, not copied. No transfer is performed yet.

GATES
- executor=0 is unchanged: every capture in every run has md5 b8f68e89c619 (trace-short x4,
  d3d11-draws x1, notice-noinput x4 captures), the same image the pre-C1 build produced.
  executor=1 produces the same md5 on all of them, i.e. C1 is a no-visual-change re-route, as a
  binding/ownership layer must be while the non-executor path binds the same surfaces.
- No measurable cost: trace-short median 33.68 / 39.79 FPS on vs 34.21 / 34.24 off (two runs
  each; the spread between runs is larger than the difference).
- Sustained Gate A scenario with executor=1: d3d11-draws pass, 2811 measured frames,
  draw_calls 140,451, median 40.64 FPS, 0 device removals; executor prepares/binds/draws equal
  and skipped=0 for all 28 statistics lines, surfaces bounded at 13-16.
- Phase 1.7 (iii) notice-noinput, which the harness previously aborted as blank: now pass, all
  four captures non-black (mean 33.453, min 0, max 255). The capture holds a real menu: 311
  distinct colours, luminance mean 32.24 / stdev 36.23, white glyphs on 0x1c171c - i.e. draws ->
  EDRAM surface -> resolve -> front buffer -> present works end to end.

NOT DONE (C2 and later, stated so nothing is overclaimed)
- Ownership transfers: the runs Claim returns are counted only. Nothing copies a previous
  owner's tiles into the claimant's surface.
- Per-owner rectangle splitting (SplitByOwner): a mixed-owner copy falls back to the copy's own
  key, which is why the 65 + 2 resolves above still read an empty surface.
- Depth tail, scaled targets (Initialize refuses scale != 1), and the readback default
  (fh1_d3d11_resolve_readback stays false).

## PHASE 1.7 — B3 ACCEPTANCE: (i) PASS, (iii) FAILS, READBACK DEFAULT STAYS FALSE (2026-10-22)

FINAL BINARY RE-CONFIRMATION (after the logging gate was moved inside the trace cvar so the
default path carries no bookkeeping; BUILD_EXIT=0): trace-short-20261023T021159Z, readback
off, trace on - resolve 0 info 000E0051, source_in_rect 1800, 921,600 of 942,080 words,
first 6F6CF1C6; resolve 1 info 000E0041, 921,600 words, first C6F16C6F. Same numbers as
014526Z.

All three acceptance runs are readback ON (fh1_d3d11_resolve_readback=true) on the fixed build
(BUILD_EXIT=0), GTX 570 by name, window hidden, state root .local/preview.

(i) notice-frames — PASS (content after the transition): notice-frames-20261023T014710Z,
  25 captures p0300..p1800, mean/nonzero of each PPM:
    300 0.000 ; 360 0.000 ; 420 61.960 ; 480 61.960 ; 540..720 0.000 ;
    780 44.087 ; 840 57.820 ; 900 44.487 ; 960 57.820 ; ... 1800 57.820 (44.487/57.820 alternating)
  So the notice is left and the later capture set is content-bearing and frame-varying. The
  readback itself reports the landed extent: "range=1C149000+3768320 pages_valid=920
  gpu_written=920 guest_first=C0000000 readback_words_nz=29440" (call 135466).
  The run was stopped by the 300 s timeout after frame ~1942 (readback costs ~6.5 FPS);
  every capture the scenario asks for (up to 1800) exists.

(iii) notice-noinput — FAIL, and not marginally: notice-noinput-20261023T020700Z,
  all four captures (600/1200/1800/2400) are 1280x720 all-zero (mean 0.000, nonzero_frac
  0.0000); the harness aborted the run with "noinput-600.ppm: blank or degenerate output".
  A second run is pointless for the verdict (the first already fails both the non-black and
  the identical-across-runs halves by being uniformly black).

(ii) long-drive — NOT RUN, because (iii) already carries a failing verdict: with the readback
  default unable to flip, a readback-on long-drive cannot change the outcome. Stated, not
  silently skipped.

WHY (iii) IS BLACK, MEASURED, AND IT IS NOT THE RESOLVE: the native resolve writes its
  source's content faithfully, and the source's content is uniform black-with-alpha. On the
  fixed build (trace-short-20261023T021030Z), the presented front-buffer surface
  0/16/1x/c2 resolves with info 000E0041 (the shader's word fast path, no decode/encode):
    call=1650 key=0/16/1x/c2 info=000E0041 len=3768320 words_nz=921600 zero=20480
    first=C0000000
  C0000000 is the source word itself (2_10_10_10: R=G=B=0, A=3) - the extent is the whole
  1280x720 rect full of one opaque-black word, not zeros and not a dropped store. The bind
  checks prove the SRV survives for the whole session now: 82 checks (first 64 dispatches,
  then every 256th) with 0 srv0=NULL, at call 0 through call 2560.
  So the readback takes the resolved extent (black) and overwrites the guest's own front
  buffer, which held a real frame (the same swap trace reads guest_first=C6F16C6F,
  guest_words_nz=28480 at the presented address before the readback replaces it). That
  surface is cleared to opaque black and the draws do not fill it: "draws -> EDRAM" is the
  Part 2 executor's job, and it is the named missing verdict.

DECISION: fh1_d3d11_resolve_readback stays FALSE (src/graphics/flags.cpp unchanged). The
  fix is not weakened for it: the resolve path now lands content whenever its source has any
  (1.6), and the missing content is upstream of the resolve.

GATE SANITY ON THE FIXED BUILD (readback off, the default end state, no pack):
  d3d11-native-resolve-path-20261023T020809Z: result pass; draw_calls 414,682; 11,684 resolve
  calls / 9,158 dispatches / 0 skipped; 0 device removals (no 887A0005); median frame
  26,831 us = 37.27 FPS against the recorded 37.29 FPS baseline for this scenario.
  The OM unbind per resolve costs nothing measurable there.

PHASE 2 STATUS AT THE TIME OF THIS SECTION: NOT STARTED (this is the state 1.7 was judged in).
  SUPERSEDED by the PHASE 2 - C1 section at the top of this file: the executor now exists, and
  with it the (iii) scenario captures a real frame (measured there with its numbers).

## PHASE 1 — FIRST RESOLVE CONVICTED AND FIXED (2026-10-22, this pass)

**VERDICT: the resolve's SOURCE SRV bind is dropped by Direct3D 11 whenever the source
surface is still bound as an output-merger view (RTV/DSV) at dispatch time. The compute
shader then samples an unbound t0 (zeros) and the resolve writes zero words over a
populated source. The defect is BIND STATE, not the CB, not endianness, not the staging
order, not the relative-base wrap.**

1.1 — THE CB AS THE GPU SEES IT (trace-short-20261023T014234Z, pack off, readback off):
  CB snapshot (CopyResource -> CPU_READ staging, taken in-stream before the dispatch,
  immediately after the upload; Map/WRITE_DISCARD path) beside the CPU-packed dwords:
    resolve 0 cpu 00000000 02D00500 01008000 01008000 00000000 004E0051 00000000 00000500
    resolve 0 gpu 00000000 02D00500 01008000 01008000 00000000 004E0051 00000000 00000500
    resolve 1 cpu 00000000 02D00500 01008000 01008000 00000000 004E0041 00000000 00000500
    resolve 1 gpu 00000000 02D00500 01008000 01008000 00000000 004E0041 00000000 00000500
  Every cell equal; no differing cell to name. dest_info differs only in endian 3:5 + swap 6
  (0x51 vs 0x41), exactly as the earlier force_endian bisect already acquitted.
  (0x004E0051 = the guest 0x000E0051 | bit 22, the unscaled-destination flag this path sets.)

1.2 — UPLOAD A/B (words written, resolve 0 / resolve 1, extent 3,768,320 B = 942,080 words):
    Map/WRITE_DISCARD, DYNAMIC (pre-fix)        0 / 921,600   (trace-short-233758Z)
    Map/WRITE_DISCARD, DYNAMIC (post-fix) 921,600 / 921,600   (trace-short-014526Z)
    UpdateSubresource, DEFAULT   (post-fix) 921,600 / 921,600 (trace-short-014628Z)
  Both upload modes now land; the upload path is not the locus.

1.3 — SESSION-FIRST OR SURFACE-FIRST: NEITHER. New diagnostic
  fh1_debug_resolve_bind_check reads CS t0/t1/u0/b0 back from the context after the binds
  and before the dispatch, so it reports the bind the GPU will see, not the one requested.
  trace-short-20261023T014234Z (pre-fix), 64 dispatches:
    call  0            srv0=NULL  uav=ok cb=ok  info=000E0051  dest 0 of 942,080
    calls 1-13         srv0=ok    uav=ok cb=ok  (content where the source had any)
    calls 14-30        srv0=NULL  (owner 0/16/1x/c3 and 0/32/4x/c12)
    call  31           srv0=ok (owner c0) / call 32 srv0=NULL (owner c2)
    calls 39-55        srv0=NULL ; 56 ok ; 57 NULL ; 58 srv0=NULL srv1=NULL (depth)
  So: not the session's first resolve (call 31 fails later; call 0 fails as the first),
  not the surface's first resolve (call 14 is seen=1 and fails, call 31 seen=1 and lands),
  and not the swap/endian class (call 32 is 0x41 and fails; call 31 is 0x50 and lands).
  The log's own `seen` column (first 64 resolves, per-resolve key) carries the same verdict:
  the failure tracks the bind, and the bind tracks whether the surface is still the current
  render target. Confirmed by the same resolve measuring source_in_rect=1800 in-rect
  non-zero texels while writing 0 of 942,080 words, and by uav/cb reading ok in every one
  of those dispatches.

1.4 — STAGING ORDER AND WRAP: BOTH CLEAN BY CONSTRUCTION/MEASUREMENT.
  Order in the dispatch setup (src/graphics/d3d11/command_processor.cpp IssueCopy ->
  fh1_native_resolve.cpp ResolveToMemory):
    EnsureResolveStaging(len)  (:1220, BEFORE any view exists for this dispatch)
    RequestRange(extent)       (mirror current)
    GetOrCreateSurface         (source)
    [source in-rect probe readback, diagnostic only]
    ResolveToMemory: GetConstantsBuffer -> SRV create -> CB upload (Map or UpdateSubresource)
                     -> CSSet* (bind) -> Dispatch -> unbind
    ReadbackResolveStaging (after WaitForGpu) -> CopyResolveStagingToHost
  Ensure cannot reallocate after this dispatch's UAV exists: the UAV is created inside
  Ensure and the staging buffer only grows on a later, larger extent, which is a later
  dispatch. Measured wrap: dest_base < copy_dest_extent_start occurred 0 times in 600
  logged resolves on the 010807Z run (wrap=1 count 0) and 0 times in the first 64 resolves
  of every run this pass; dest_base == extent_start (relative base 0) in every one of those
  zero-word resolves. The relative-base wrap is ACQUITTED with counts.

1.6 — FIX (one hunk, at the convicted level): before creating and binding the resolve's
  source SRVs, ResolveToMemory unbinds the output merger
  (OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, nulls, nullptr)). Direct3D 11
  never reports the SRV/RTV conflict it resolves by dropping the SRV bind; the draw path
  leaves the surface it just rendered to bound there, and BindDrawRenderTargets re-binds its
  targets on every draw, so nothing depends on the OM state surviving a resolve.
  PAIRED REGRESSION (trace-short-20261023T014526Z, same scenario/env as 233758Z):
    resolve 0: info 000E0051, source_in_rect 1800 -> 921,600 of 942,080 words non-zero,
               first 6F6CF1C6 (8in32 of the C6F16C6F its neighbour writes - the shader's own
               endian swap, not a second CPU pass)
    resolve 1: info 000E0041, same surface -> 921,600 words, first C6F16C6F
    bind checks: 0 of 64 srv0=NULL after the fix (was 43 of 64 before it)
    resolves whose source is genuinely empty (source_in_rect=0) still write 0 - correct.

FALSIFIED THIS PASS: CB upload path (CPU==GPU dword-for-dword, and both upload modes land);
  CB contents/dest_info (0x51 vs 0x41 differ only in endian+swap, both land now); endianness
  (prior bisect, unchanged); pack/format class (the same pack now writes); staging growth and
  realloc ordering (Ensure precedes view creation; resolve 0 lands on the first dispatch);
  relative-base wrap (0 occurrences, and the failing resolves had relative base 0); UAV bind
  loss (uav=ok in every failing dispatch); "first dispatch ever" (repeat experiment + call 31).

DEAD END TO NOT REPEAT: poisoning the staging buffer with UpdateSubresource on the
  UAV/ALLOW_RAW_VIEWS buffer crashes the Fermi UMD deterministically
  (0xC0000005 in nvwgf2umx, same crash id both runs, at the first resolve). Removed; a
  sentinel fill must go through a copy from a non-UAV pattern buffer if it is ever wanted.

FILES THIS PASS: include/rex/graphics/d3d11/fh1_native_resolve.h (+bind_checked_dispatches_),
  src/graphics/d3d11/fh1_native_resolve.cpp (OM unbind before the source SRVs; bind-check
  log; CB capture HRESULT reporting), src/graphics/d3d11/command_processor.cpp (per-resolve
  log line with key/seen/zero counts and the first-64 gate), include/rex/graphics/flags.h +
  src/graphics/flags.cpp (fh1_debug_resolve_bind_check). Build BUILD_EXIT=0.

## STEP C REVISED — C0' AUDIT DONE, C1 STOPPED ON A FALSIFIED PREMISE (2026-10-22)

C0' — THE FACTS THE BUILD WOULD STAND ON (file:line, HEAD):
1. D3D12 executor structure (the structural spec): class Fh1NativeExecutor in
   include/rex/graphics/d3d12/fh1_native_executor.h:64 with PrepareTargets (:79),
   BindTargets (:97), NativeDrawIssued (:99), NativeResolve (:102), FlushResolveReadbacks
   (:103), OnSwap (:108), LogStats (:80), and the frame dumper behind
   frame_dump_->RecordDrawInputs/RecordCopyInputs (:84-93). Construction/init/teardown in
   d3d12/command_processor.cpp:1507 (make_unique), :1515 (Initialize), :1580/:1593/:1720 (reset).
   Capture/claim call sites: PrepareTargets :2429, BindTargets :2432, RecordDrawInputs :2503,
   NativeDrawIssued :2787 and :2848, RecordCopyInputs+NativeResolve :2893-2894, OnSwap :1895,
   FlushResolveReadbacks :1561 and :2301.
2. Fh1EdramTiles is ALREADY SHARED — include/rex/graphics/fh1_edram_tiles.h:17, no D3D12-local
   copy. The prompt anticipated a hoist as a flagged shared-code change; it is not needed.
3. Cvar home confirmed: include/rex/graphics/flags.h (REXCVAR_DECLARE) + src/graphics/flags.cpp
   (REXCVAR_DEFINE). fh1_d3d11_resolve_readback is flags.cpp:39; the Part 1 probes are beside it.
4. Step B dump cvars: DEFINED at src/graphics/d3d12/fh1_native_executor.cpp:33 (dump_frames),
   :36 (dump_dir), :39 (readback_new_resolves) and NOT declared in any header. Route for a D3D11
   TU: add REXCVAR_DECLARE lines for the three to include/rex/graphics/flags.h (additive) and read
   them with REXCVAR_GET; the definitions stay in the D3D12 TU (one definition, no duplicate).
5. D3D11 packet flow: there is no FLIP concept on either backend — the swap packet is the frame
   boundary. Shared base: src/graphics/command_processor.cpp:2591-2610 (RegisterSwapCommand reads
   frontbuffer ptr/w/h and calls the virtual IssueSwap), so D3D11's insertion point for OnSwap-style
   work is its IssueSwap (src/graphics/d3d11/command_processor.cpp:1656). Draws enter IssueDraw
   (d3d11/command_processor.cpp:893) and bind native targets in BindDrawRenderTargets (:303,
   called at :992) — the capture/claim points for a D3D11 executor are exactly there. Copies enter
   IssueCopy (edram_mode kCopy from IssueDraw :905) into the resolve plan path whose dispatch is
   ResolveToMemory (d3d11/command_processor.cpp:1350 -> fh1_native_resolve.cpp:142).

MEASUREMENT CORRECTION (why the premise was re-tested): every "extent is linear" figure in the
earlier Part 1 entry counted one byte per 16/64. On a destination written with 8in32 byte order that
samples a zero byte of a written word, so a fully written, byte-swapped extent reads as blank. All
counts below are non-zero WORDS.

RE-TEST OF "THE DRAWS NEVER FILL THE FRONT BUFFER" (trace-short-20261022T233758Z, paired per
resolve — source-surface readback and destination staging readback taken inside the same resolve):
  resolve 0: info 000E0051, surface 0/16/1x/c2, source in-rect content 1,800 sampled non-zero texels
             -> destination 0 of 942,080 words.
  resolve 1: info 000E0041, SAME surface, same rect 0,0-1280,720, same pitch, same dest_base 0
             -> destination 921,600 words.
  Forced endianness (fh1_debug_resolve_force_endian, new diagnostic): forcing resolve 1 from endian 0
  to endian 2 still writes 921,600 words (bytes swapped: first 6F6CF1C6 instead of C6F16C6F).
  Forcing resolve 0 to endian 0 changes nothing. So ENDIANNESS IS INNOCENT, and dest_info is not the
  difference: resolve 0 fails as the SESSION'S FIRST RESOLVE.
  Repeating that first dispatch with identical state in the same submission does NOT make it land
  (experiment made, measured, reverted) - it is not a first-use-lost dispatch either. What differs is
  not in the constants the dispatch log prints (shader kind, rect, groups, dest_base, dest_pitch,
  src_srv_format, owner layout are byte-identical between the two). Named open question, not a guess.

WHAT THE RUNS SAY ABOUT CONTENT (notice-frames-20261022T233025Z, readback on, word counts):
  21,519 readbacks land 0 words; 1,982 land 8,580; 101 land 8,580; 86 land 16,928; 61 land 32,768;
  6 land 28,800 with first=C6F16C6F (the boot frame, nearly the whole 3.7 MB extent).
  So the store path, the copy and the readback all carry real pixels — 2,000+ extents do. The front
  buffer at the swap is NOT one of them: the swap trace at every sampled swap reports guest_words_nz=0
  for the presented address, and the last resolve for that address is 000E0041 in the same frame.

VERDICT: C1 is NOT BUILT. The executor's value rests on "draws do not fill the front-buffer surface",
and that premise is contradicted on the one resolve where both sides were measured together (a surface
with 1,800 in-rect non-zero texels produces a blank destination). Building a new subsystem on top of a
resolve that can blank a populated surface would put ~2,000 lines between the defect and its evidence.
Stopped here per the prompt's audit-then-report rule; C2/C3 and Gate C were not started, no executor=1
claim is made, and no scenario file was edited. The next experiment is named: instrument the first
resolve's constants buffer *as the GPU sees it* (read back the CB through a staging copy, or replace
the Map/DISCARD with UpdateSubresource) and diff it against resolve 1's — the difference is in that
buffer or in the surface's first-view state, not in dest_info, the store, the copy or the readback.

FILES THIS PASS: d3d11/command_processor.cpp (paired source/staging probes, word-granular counts at
resolve and at the swap, draw-RT key tracking, rt_bound/samples/sample_select/info on the resolve
trace, fh1_debug_resolve_direct_mirror A/B switch), d3d11/fh1_native_resolve.cpp (endianness bisect
cvar), include/rex/graphics/d3d11/command_processor.h (probe members), flags.h/flags.cpp
(fh1_debug_resolve_force_endian, fh1_debug_resolve_direct_mirror). Build clean (BUILD_EXIT=0).


## PART 1 — THE MIRROR STORE IS FIXED; THE FRONT BUFFER IS STILL EMPTY (2026-10-22)

JOB A (size discriminator, recorded before this entry): the 0xDEADBEEF store at the top of the
resolve shader lands in a dedicated 4 MB raw UAV bound through the same ResolveToMemory dispatch
(DEADBEEF read back), and never lands in the 512 MB shared-memory mirror at any guest front-buffer
address (0x1DAC5000, 0x1BDB1000, 0x14085000), while it does land at mirror offset 0. Raw UAV
*windows* over the mirror cannot be created at all: CreateUnorderedAccessView returns E_INVALIDARG
for every FirstElement != 0 (GetHostBufferUAVWindow logs it), so "window the 512 MB view" is not
available on this driver. Size bisect with the marker store: 512 MB never lands at the front-buffer
address, 4 MB lands - the mirror view, not the resolve code, was the defect.

JOB B1 — FIX (implementation): resolves no longer dispatch into the mirror's raw UAV. The
destination of every non-diagnostic resolve is a dedicated DEFAULT buffer with
D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS and a full-range raw UAV (1 MB, doubling, capped at
64 MB - D3D11SharedMemoryDeviceUploadSink::EnsureResolveStaging), whose stores DO land on the GTX
570. The shader is given the destination base RELATIVE to the extent's start (dest_base -
copy_dest_extent_start), because the staging buffer holds the extent from offset 0; after the
dispatch that extent is copied into the mirror at its guest byte address with CopySubresourceRegion
(CopyResolveStagingToHost) - a copy-engine operation with no shader-address limit. Files:
src/graphics/d3d11/shared_memory_upload_device.{h,cpp} (staging buffer + UAV + copy + staging
readback), src/graphics/d3d11/command_processor.cpp (target selection, relative base, copy, trace
probes), include/rex/graphics/flags.h + src/graphics/flags.cpp (probe cvars). The IssueCopy SRV
unbind stays exactly as fixed (the mirror's SRV at VS/PS t0 is unbound before every resolve).

B2 — PROOF AT THE MIRROR: probe run trace-short-20261022T213115Z (marker build,
REX_FH1_DEBUG_RESOLVE_STORE_PROBE=true, REX_FH1_SWAP_FETCH_TRACE=true, pack on):
  call 0, info 000E0051, range 1DAC5000+3768320 - staging first=DEADBEEF (nonzero sampled 1) AND
  mirror readback first=[DEADBEEF 00000000 ...] at 1DAC5000. The same call before the fix never
  received a byte.
  call 1, info 000E0041, range 1C149000+3768320 - staging first=C6F16C6F, 229,795 / 235,520 sampled
  nonzero bytes, and the mirror readback matches: real resolved CONTENT reaches the guest-memory
  front-buffer address (not just the marker).
The 0x000E0051 endian-swap case therefore stores; the earlier "8in32 emits zeros" reading was two
facts superimposed (the store never landing + an empty source surface).

B2 — WHAT IS STILL ZERO, AND WHY (measured, not inferred): with the probe shader replaced by one
that stores LoadOwnerWord(pixel,0,0) at offset 0 (draws-20261022T220355Z), call 0 (0x0051) reads
back 0 - the SHADER samples zero at that moment - while call 1 (0x0041) reads C6F16C6F from the
same surface key (0/16/1x/c2). So the remaining zeros are empty resolve SOURCES, not lost stores.
In long-drive (fh1-long-drive-20261022T215333Z-state, readback on) 4,379 of 43,866 readbacks land
NONZERO content (the 0x1CE95000 and 0x1CA95000 extents consistently), while every front-buffer
extent (1C4E1000, 1C621000, 1DAC5000, 1DD45000, 1CE2D000) lands zeros: the draws that fill those
EDRAM surfaces are the Part 2 executor's job, and no scenario presents in-world content until it
exists. Stated plainly: the STORE is fixed, the PIXELS are not there yet.

B3 — THE SWAPFETCH ACCEPTANCE (readback=true, pack on) - NOT MET, default NOT flipped:
 - notice-frames (notice-frames-20261022T213234Z): 26 captures, every one mean 0.000, distinct 1
   (black). Required: post-transition content. FAIL.
 - fh1-long-drive (fh1-long-drive-20261022T214357Z): event-ready and event-entered both mean 0.000,
   distinct 1. Required: in-world captures differing. FAIL.
 - notice-noinput (notice-noinput-20261022T221732Z): 4 captures byte-identical (sha 822e3a311bc3)
   - the byte-identity half passes, but they are black, so it passes vacuously. FAIL on content.
Consequence: fh1_d3d11_resolve_readback stays FALSE (flags.cpp:39). The missing verdict is named:
resolves land, but the front buffer's EDRAM sources are not filled by the draws, so reading the
mirror back into guest memory replaces the CPU-side boot frame with zeros. The old readback-off
behaviour (a frozen but plausible boot frame) is the better lie, and is what stays shipped.

B4 — PERFORMANCE (same binary, A/B, draws scenario, no trace): staging path 34.19 FPS median
(29,248 us, 114,704 draws) against fh1_debug_resolve_direct_mirror=true 37.09 FPS (26,962 us,
125,087 draws) - the correct path costs ~8%, and part of the direct path's advantage is that its
stores are dropped and write no memory at all. The Step E reference (42.17 FPS) was measured on
exactly that non-writing path, so it is an optimistic baseline, not a like-for-like number.
REGRESSION FLOOR (four-gate suite, .local/muse_gates_all.ps1, this build, pack off, readback off):
GATES_PASSED 3 of 4 - cleared-frames pass (115,282 draws), native-resolve-path pass (426,067
resolves, 0 skipped), draws pass (117,653 draws); texture-uploads FAILS its gate on the upload
tally (11,000 < the 12,000 floor that was calibrated on the faster, non-writing resolve path).
Artifacts: d3d11-{cleared-frames,texture-uploads,native-resolve-path,draws}-20261022T2205-2208Z.

B5 — THE CONTRACT PART 2 INHERITS (one paragraph, implementable without re-deriving):
On Fermi/driver 391.35 the 512 MB mirror's raw UAV accepts stores only at low offsets, and no
sub-range raw view of it can be created. Therefore every resolve writes its destination extent into
the resolve staging buffer (a 4 MB, doubling, <=64 MB DEFAULT ALLOW_RAW_VIEWS buffer, full-range
raw UAV), with fh1_dest_base passed as dest_base - copy_dest_extent_start so the shader's tiled
layout math produces offsets that begin at 0 in that buffer; after the dispatch the extent is
copied into the mirror at copy_dest_extent_start with CopySubresourceRegion, and only then is the
range marked resolved/invalidated and (with fh1_d3d11_resolve_readback) pulled back to guest RAM.
The shader keeps dest_info handling exactly as-is: pack/endian/swap/bias/bpp are applied at the
store (pack 0..5, endian 0..3 8in32 semantics, red/blue swap bit 6, exponent bias 8:15), so a
Part 2 executor writing through this path gets the guest's byte order for free; it must not add a
second endian or pack step. Two things ride along: the SRV at VS/PS t0 must be unbound before the
resolve's UAV is bound (D3D11 silently drops a UAV bind on a resource already bound as an SRV), and
the staging buffer is reused across resolves, so nothing may read it between a dispatch and its
copy. Cost: ~8% of the frame on the draws scenario, all of it the copy that makes the stores land.

## PART 2 — STEP C EXECUTOR: AUDITED, BLOCKED (2026-10-22) — SUPERSEDED by PHASE 2 C1 above

C0 AUDIT (facts at HEAD, file:line):
- fh1_native_executor is NOT a cvar in this tree: `grep -rn "fh1_native_executor"` over src and
  include returns only the C++ member/class names (d3d12/command_processor.cpp:1507-1593,
  1895, 2301, 2429, 2894; include/rex/graphics/d3d12/fh1_native_executor.h). No
  REXCVAR_DEFINE_INT32 for it exists, and no toml sets it. The value the command processor
  "uses" is therefore nothing: the {0=off,1=on} contract has no implementation to flip.
  IssueSwap's kNativeExecutor (d3d11/command_processor.cpp:1612) is only the presenter tag.
- There is NO D3D11 executor: src/graphics/d3d11/ contains no fh1_native_executor.*; the graphics
  CMakeLists lists only vulkan/fh1_native_executor.cpp:67 and d3d12/fh1_native_executor.cpp:117.
  Step B's scaffolding is D3D11Fh1NativeSurfaces (real) and D3D11Fh1NativeResolve (real, the file
  this part's Part 1 work lives in), not an executor.
- ResolveToMemory's D3D11 callers at HEAD: exactly one - D3D11CommandProcessor's resolve plan
  path (command_processor.cpp:1350), which is reached from the guest's copy packet (the same path
  that already bumps resolve_call_count_ and marks the range resolved). There is no FLIP-driven
  chain, because there is no FLIP.
- D3D12's readback contract (for the counterpart C2 asks for): fh1_native_executor.cpp:1582-1605
  (kMinLength 256 KB, fh1_native_readback_new_resolves default true) with the copy flushed by
  FlushResolveReadbacks (fh1_native_executor.cpp:1694, called from d3d12/command_processor.cpp:
  1561/2301). No D3D11 equivalent exists or is missing-in-place: the D3D11 readback is Part 1's
  fh1_d3d11_resolve_readback block (command_processor.cpp:1479 onward).
- Step B dump cvars: fh1_native_dump_frames, fh1_native_dump_dir and
  fh1_native_readback_new_resolves are DEFINED (not declared-only) in
  src/graphics/d3d12/fh1_native_executor.cpp:33-40. A D3D11 executor TU must therefore NOT define
  them again (duplicate cvar definition) - the audit's correction to the plan's
  "DECLARE-not-DEFINE" premise is that they already exist once, in the D3D12 TU.

VERDICT: Part 2 reports BLOCKED BY PREMISE, not by Part 1. C1 requires a native-kernel executor
(EDRAM tile capture, per-tile ownership, CPU emulation of uncaptured kernels, fallback ordering)
that does not exist for this backend at all - it is a new subsystem, not a wiring job, and it was
not written in this pass. C2/C3 are downstream of C1 and were not started. No claim is made that
Gate C, depth tail or executor=1 anything was run.

## PART 3 — D3D12 PARITY: BLOCKED

Prerequisite (Gate C / a working executor) does not hold, so this part is a one-paragraph entry:
D3D12 parity is blocked by Part 2, which is blocked by its own premise (no D3D11 executor exists).
No catalog was rebuilt, no parity run was made, and no MAE or counter comparison is claimed here.

## EMPTY RESOLVE — the store never reaches the mirror (2026-10-22)

TRACE TABLE (one probe run; .local/probes/trace-short.fh1test, REX_FH1_SWAP_FETCH_TRACE=true,
REX_FH1_D3D11_SHADER_PACK=true; every figure below is from the probe's own logs):
 call dest range+len              dest_info (pack/endian/swap/bias/bpp)  surface     mirror before -> after            changed
 0    1DAC5000+3768320           000E0051 (1 / 8in32 / 1 / 0 / 2)        0/16/1x/c2  67C67AC8…/nz=0   -> same /nz=0      no
 1    1BDB1000+3768320           000E0041 (1 / none  / 1 / 0 / 2)        0/16/1x/c2  2F18B91F…/nz=229795 -> same      no
 2    14085000+3768320           000E0050 (0 / 8in32 / 1 / 0 / 2)        0/16/1x/c0  A49638A8…/nz=235520 -> F146E4E0…/nz=5120 changed
 3    14085000+3768320           000E0050 (0 / 8in32 / 1 / 0 / 2)        0/16/1x/c0  F146E4E0…/nz=5120 -> same          no
Decoded from the constants actually sent: rect 0,0-1280,720; dest_base == extent start in every
row; dest_pitch 1280; dispatch 160x90x1 of the SM 5.0 resolve_memory color variant (kind=0,
msaa=false, src_srv_format 24 = R10G10B10A2 for c2, 28 = R8G8B8A8 for c0).
MEASUREMENT TRAP FOUND AND FIXED IN THE PROBE: a Map on the readback staging buffer waits only for
its own CopySubresourceRegion, not for the compute work queued before it - the first probe read
"after" == "before" even when a later synchronised read saw different bytes. The probe now calls
WaitForGpu() before every hash (the shipped readback block always did).
SOURCE SURFACE CONTENT (same run, sampled readback of the owner texture): 1280x2048, first=C6F16C6F,
8240 non-zero sampled texels for the front-buffer resolves - so the surface has content while the
destination stays untouched.

THE DECIDING EXPERIMENT (not a hypothesis - a direct test): an unconditional
  fh1_memory.Store(fh1_dest_base, 0xDEADBEEFu);
was placed at the top of the resolve shader's main(), before every branch, in
fh1_native_resolve_memory.cs.hlsl, recompiled with fxc //T cs_5_0 //O3 into
bytecode/d3d11_5_0/fh1_native_resolve_memory_color_cs.h (the checked-in header otherwise regenerates
BYTE-IDENTICAL from the current source, so the shipped bytecode is not stale), rebuilt, and the run
repeated with the readback on: the readback of the resolve's own extent still reads
first=[00000000 00000000 00000000 00000000], on a healthy device, over 12k dispatches. The store
never reaches the mirror. The shader and its pack branches are NOT the defect. (Shader restored from
/tmp backups; DEADBEEF is in neither the .hlsl nor the .h now.)

CANDIDATES, FALSIFIED ONE BY ONE:
 (a) extent / dest-range arithmetic yields zero bytes - FALSIFIED. Length is 1280*736*4 =
     3,768,320, dest_base == extent start, and forcing pack 0 changes bytes at the same address.
 (b) the 8in32 shader variant's store is compiled out or stubbed - FALSIFIED. The bytecode
     regenerates byte-identically from source; forcing pack 0 makes a store land at that address;
     the marker store above bypasses every pack branch and still lands nowhere.
 (c) the resolve is classified as a transfer and routed away - FALSIFIED. IssueCopy reaches
     ResolveToMemory (12,652 dispatches, 0 skipped in the resolve-path gate) and the dispatch log
     shows 160x90 groups per 1280x720 resolve.
 (d) the store fires into a range the readback does not read - FALSIFIED by a whole-mirror sweep
     (512 MB fingerprinted in 4 MB chunks around the resolve, first 3 occurrences): no chunk changes
     at the resolve's own call; the chunks that do change (75, 76, 80, 81) are other traffic between
     calls.
 (e) SRV/UAV aliasing - the draw path binds the mirror's SRV at VS/PS t0 for every draw
     (command_processor.cpp:846-848), so the same resource is an SRV when the resolve binds it as a
     UAV. Direct3D 11 drops such a UAV bind silently. Tested: the draw stages' view is now unbound
     before every resolve (kept - it is correct hygiene) and the outcome did NOT change, so this is
     not the cause either.

WHAT SURVIVES: the dispatch is issued with a live-looking UAV and correct constants, but its UAV
writes never land in the 512 MB mirror on this GTX 570, while the device stays healthy and the
per-resolve CPU bookkeeping (12,652 dispatches) is right. The remaining named suspect is the UAV
VIEW itself: host_buffer_uav_ is a raw UAV with NumElements = size/4 = 134,217,728 over a 512 MB
DEFAULT buffer. Next test is exact - bind a SMALL dedicated raw UAV (4 MB) through the same
constants path and store a marker into it: if that lands, the 512 MB raw view is unusable on this
device/feature level and the mirror UAV must be created smaller (or in chunks); if it does not, the
dispatch itself is not executing and the shader stage binding is the suspect.

CONSEQUENCE (unchanged from the SwapFetch verdict, now with the cause one level deeper): the
readback bridge must stay OFF by default. It is not copying resolve output; it copies whatever the
mirror holds, and for pages the upload never touched that is zeros - which is exactly the black
screen measured with fh1_d3d11_resolve_readback=true. Every "content" readback seen earlier was CPU
upload traffic, not the resolve.
Job B (2)/(3) of the SwapFetch prompt therefore still cannot pass; Job C still unreachable.

REGRESSION: four-gate suite with the pack enabled, after the unbind change:
 GATES_PASSED=4, all RUNNER_EXIT=0 / GATE_EXIT=0:
 - d3d11-cleared-frames-20261022T205938Z: pass, 124,930 draws, 36.24 Hz, median 27,199 us
 - d3d11-texture-uploads-20261022T210058Z: pass, 124,202 draws, uploads 12,000
 - d3d11-native-resolve-path-20261022T210218Z: pass, 452,344 draws, resolves 12,652, 0 skipped
 - d3d11-draws-20261022T210258Z: pass, 125,959 draws, presented title frame

## SWAP FETCH TRACE — the presented frame is not the swap's fault (2026-10-22)

TRACE FIRST, as the prompt requires (new cvar fh1_swap_fetch_trace, default off; per swap it logs
the packet's front buffer pointer, texture fetch constant 0 as the guest wrote it, the key base it
derives, the first guest word at that base, and the last native resolve's guest range/pitch/info and
how many swaps ago it ran). Probe: .local/probes/trace-short.fh1test and notice-frames.fh1test,
REX_FH1_SWAP_FETCH_TRACE=true REX_FH1_D3D11_SHADER_PACK=true, state root .local/preview.
VERBATIM (notice-frames run 20261022T195021Z, swaps 0-4):
  swap=0 fb_ptr=1BDB1000 packet=1280x720 fetch0=[8A000002 1BDB1036 0059E4FF 00001414]
    base=1BDB1000 pitch=40 tiled=1 fmt=54 | last resolve call=0 range=00000000+0
  swap=1 fb_ptr=1BDB1000 ... base=1BDB1000 | last resolve call=1 range=1DAC5000+3768320
  swap=2 fb_ptr=1C149000 fetch0=[8A000002 1C149036 ...] base=1C149000 | call=1 range=1DAC5000
  swap=3 fb_ptr=1BDB1000 base=1BDB1000 | call=1 range=1DAC5000
  swap=4 fb_ptr=1C149000 base=1C149000 | call=1 range=1DAC5000
Over the full 2,502-swap run: resolve ranges 1BDB1000+3768320 x1210, 1C149000+3768320 x1210,
1DAC5000+3768320 x81. So the descriptor CHANGES every swap (double-buffered front buffers, matching
the packet's fb_ptr exactly) and the resolve path writes those same two guest ranges.
NEITHER 1(a) NOR 1(b) HOLDS. The swap fetch, the texture key derivation and the resolve destination
are all correct. The mechanism is a THIRD one:
  1. The D3D11 native resolve writes into the GPU MIRROR of shared memory (a DEFAULT buffer with a
     raw UAV), not into guest memory. Nothing in the D3D11 backend ever read the mirror back
     (no CopyReadbackToGuest caller anywhere in src/graphics/d3d11), so the front buffer in guest
     memory kept whatever the CPU/guest last wrote there - the boot legal notice - while 184k
     resolve dispatches per run wrote frames nobody read.
  2. RequestSwapTexture then re-untiles that stale guest memory every swap and the presenter paints
     it: a live-looking pipeline showing a frozen image.

FIX LANDED (readback + trace; readback behind a cvar, see the verdict):
- src/graphics/d3d11/shared_memory_upload_device.{h,cpp}: ReadbackFromHostBuffer(offset, dest, len)
  through a CPU-readable staging buffer (chunked, kSharedMemoryUploadChunkBytes), created on first
  use and released in DestroyHostBuffer.
- src/graphics/d3d11/command_processor.{h,cpp}: after a completed resolve, WaitForGpu, read the
  resolve extent back, shared_memory_->CopyReadbackToGuest it (the pages are already GPU-written
  and valid from MarkRangeAsResolved), and tally readbacks/bytes. New cvar
  fh1_d3d11_resolve_readback (default FALSE), fh1_swap_fetch_trace (default false) in flags.{h,cpp}.
- The trace also reports the count of valid vs GPU-written pages and the first guest word after the
  copy, so a readback that silently skips is visible.
VERIFIED (REX_FH1_D3D11_RESOLVE_READBACK=true, trace on, trace-short run):
  "FH1 resolve readback landed: range=1BDB1000+3768320 pages_valid=920 gpu_written=920
   guest_first=C6F16C6F source_first=C6F16C6F" - the resolved frame reaches guest memory, and the
  untiling upload that follows reads it back (swap upload source scratch_first=C6F16C6F).

WHY THE READBACK IS OFF BY DEFAULT - the blocker behind it:
With the readback on, the title presents BLACK (all captures uniform 0.0, distinct values 1, the
runner rejects t0300 as "blank or degenerate output"), including on the notice screen with no input
at all. The D3D11 native resolve produces no pixels: front-buffer resolves of the notice surface
(which demonstrably holds content - a readback of the surface texture gives first=C6F16C6F and
230,400 non-zero sampled words) still write an ALL-ZERO extent, and the raw readback of a 1280x720
resolve is zero in every sampled word. Pre-transition resolves (dest_info 0x000E0051, endian 8in32)
produced content in one run; post-transition ones (dest_info 0x000E0041, endian none) produced zeros
in every observed run - both decodings must store a word, so the destination write itself is not
happening there, not just decoding wrong. That is a resolve-content defect inside the D3D11 native
executor (the same code path Step C rebuilds), not the swap fetch, and the D3D12 backend presents
white on the same build, so it is not D3D11-only.
Also note for the record: ChangeLog/tree contradiction - D3D11Presenter implements CaptureGuestOutput
only; the D3D12 presenter does NOT implement CapturePresentedOutput (grep shows only Vulkan does), so
the prompt's "read back the presented buffer the way the D3D12 presenter does" has no referent.

CONSEQUENCE FOR THE JOB: Job B (1) control passes (notice-noinput, 4 captures, 42 distinct values,
mean 33.451, byte-identical - correct: the title must not advance without input). Job B (2) and (3)
CANNOT pass: with truthful presentation the images are black; with the cvar off they are the frozen
notice. Job C is not reachable. No scenario was edited.

REGRESSION: four-gate suite with the pack enabled, readback defaults (off):
 GATES_PASSED=4, RUNNER_EXIT=0 and GATE_EXIT=0 on all four (script .local/muse_gates_pack.ps1):
 - d3d11-cleared-frames-20261022T202229Z: pass, draw_calls 126,133, 36.13 Hz, median 27,073 us
 - d3d11-texture-uploads-20261022T202349Z: pass, draw_calls 126,713, uploads 12,000
 - d3d11-native-resolve-path-20261022T202509Z: pass, draw_calls 465,997, resolves 10,695, 0 skipped
 - d3d11-draws-20261022T202550Z: pass, draw_calls 125,783, presented title frame
 The cleared-frames median (27,073 us) is above the 17:49Z suite's 24,204 us; the readback and trace
 are both disabled in these runs, so the delta is machine state, not these changes - flagged, not
 explained away.

## JOB A — pack swap + four-gate suite with the pack ENABLED (2026-10-22)

Swap done: .local/fh1-d3d11.pnsp is the regenerated pack (sha256 6E34D2AA...E44D, 507,859,692
bytes); the stale one is kept as .local/fh1-d3d11.pnsp.stale-1021 (sha256 0BFDEB19...D374).
Suite: powershell .local/muse_gates_pack.ps1 (REX_FH1_D3D11_SHADER_PACK=true, every run stages
.local/fh1-d3d11.pnsp). GATES_PASSED=4, RUNNER_EXIT=0 and GATE_EXIT=0 on all four:
- d3d11-cleared-frames-20261022T183249Z: pass, draws 139,065, median 24,204 us (41.31 FPS)
- d3d11-texture-uploads-20261022T183408Z: pass, draws 139,909, median 24,247 us (41.24 FPS)
- d3d11-native-resolve-path-20261022T183527Z: pass, draws 553,459, median 27,149 us (36.83 FPS),
  pack counters 120 hits / 36 misses / 0 refused
- d3d11-draws-20261022T183606Z: pass, draws 139,106, median 24,428.5 us = 40.94 FPS
Pack effect on the draws gate: 40.94 FPS pack-on vs the recorded 42.17 FPS pack-off (median frame
24,428 us vs 23,720 us) - the pack does NOT move sustained FPS; it is ~3% slower in this
measurement. That is the expected shape: the value is removing runtime translation (and its
first-draw stalls), not the steady-state rate, and the create-probe that guards against a stale
pack creates and discards one shader object per precompiled entry at translation time
(LoadPrecompiledBinary itself only stores the bytes; the live shader is created at pipeline bind),
so startup pays one extra driver create per shader. Do not cite the pack as a frame-rate
improvement.

## JOB A — pack regeneration (2026-10-22)

RECON. NOTES-tab2.md read; corpus inputs it left on disk exist: work-tab2/.local/tab2-scratch/
fh1-corpus/corpus.blob (+ fh1-corpus.json), same extractor output the 24,437-entry pack was built
from. Producer: .local/rexglue/src/graphics/tools/fh1_pack_producer_main.cpp, CMake target
rexgpu-fh1-pack-producer (CMakeLists.txt, EXCLUDE_FROM_ALL), needs REXGLUE_USE_D3D12 + the
D3D11 backend. dxbc.h fix mtime 2026-10-22 08:38:58 (-0600); producer rebuilt 12:13:11 (-0600),
so the binary postdates the fix.
TREE DEFECT 1: rexgpu-fh1-pack-producer compiled the Vulkan SPIR-V translator sources
(REXGLUE_USE_VULKAN=ON in this build) but was absent from the glslang/VMA link in
src/graphics/CMakeLists.txt, so it failed with "'SPIRV/SpvBuilder.h' file not found". Fixed by
adding it to that foreach (one line, same sources, same reason as rexgpu-fh1-producer).
TREE DEFECT 2: the documented build recipe (preset win-amd64-release + -DREXSDK_DIR) no longer
passes cmake/PinyonShiftRexGlue.cmake:90's -msse4.1 guard on this machine: the preset's
cacheVariables reached CMake (they are printed) but CMAKE_C_FLAGS/CMAKE_CXX_FLAGS ended up empty
in the cache and the configure aborted. Repro: plain `cmake --preset win-amd64-release
-DREXSDK_DIR=...` fails; adding -DCMAKE_C_FLAGS=-msse4.1 -DCMAKE_CXX_FLAGS=-msse4.1 passes.
.local/muse_build_game.bat now passes those two flags plus the absolute toolchain clang
(.local/toolchain/llvm-20.1.8) so the evidence binary keeps the compiler of the 17:49Z gates.

PRODUCED (offline, no GPU): producer --corpus <tab2 corpus> --output
.local/pack-regen/fh1-pack-d3d11-v2 --backend d3d11 --threads 16 -> 10,203 vertex + 14,194 pixel
variants, 0 translation failures, 40 geometry shaders, 4 runtime variant pairings not in corpus,
0 rejected entries, dxil.blob 24,437 entries. Pack built by tools/native-shader-pack.py build:
- file .local/fh1-d3d11.pnsp (was fh1-d3d11.pnsp.stale-1021, kept, not deleted)
- size 507,859,692 bytes; pack_sha256 6E34D2AAB6017A97D3D31C8289F672167E995E20F214C484C02CDB140CEBE44D
- content_sha256 9785CED0A3FABB97D23BC414EF29C1FFA7AF686B9C3BDB05D0171CAF12EAC968
- entries 24,437 (identical to the stale pack's 24,437); 40 geometry shaders; backend d3d11;
  device_features 01; flags 08; 1x1; translator 20260827.

VERIFY 3a: entry count 24,437 = stale 24,437, no delta to explain.
VERIFY 3b: entry 1290 (guest 20A41D46F34D238E, mask 7, vertex, 2 textures/1 sampler) extracted
from both packs (.local/pack-regen/{stale,v2}-20A41D46.dxbc) and disassembled with the Windows
SDK fxc /dumpbin. Stale: `dcl_constantbuffer CB1[0]`, `CB0[0]`, `CB3[0]` (size read from the wrong
dword - the Fermi device removal signature). New: `dcl_constantbuffer CB1[11]`, `CB0[30]`,
`CB3[48]` - the 2D (register, size) operand fxc emits. Bytecode 11,160 -> 10,932 bytes.
FINDING (named, not a detail): the two packs differ beyond that operand. The stale entry carried
a `dcl_uav_raw u0` + `if_z cb0[0].x` branch choosing between the mirror's UAV and t0 for ld_raw;
the new entry reads t0 directly (48 diff lines). Entry count and configuration are identical, so
this is translator code generation between the two trees (the stale pack came from work-tab2),
not a corpus difference. The new pack is the one this tree's runtime would translate on a miss.
VERIFY 3c (live, d3d11-draws-20261022T183043Z, GTX 570, pack staged + cvar on): result pass;
"Loaded 24437 FH1 precompiled shaders ... (translator 20260827, features 01, flags 08, 1x1)";
counters verbatim "FH1 precompiled shader cache: 22 hits, 7 misses, 0 entries the device refused"
at 600/1200/1800 swaps and "22 hits, 9 misses, 0 refused" at 2400 swaps; zero refusals, zero
device removals, zero forbidden log patterns. NOTE: the title hard-exits on termination, so
ShutdownContext never runs and the shutdown summary never printed - the counters now also ride
the periodic presentation line (D3D11PipelineCache::LogPrecompiledShaderStats, called from
LogSwapWait). Median 41.36 FPS in that run vs 42.17 recorded pack-off.

## JOB C — D3D12 catalogs built; parity compared, NOT a shading comparison (2026-10-22)

PRODUCER (identified from the D3D12 backend's load error and the toolchain):
 tools/produce-fh1-artifacts.ps1 builds rexgpu-fh1-producer (the in-game D3D12 producer plugin) and
 pinyon_shift_fh1_archive_extract, extracts the disc shader corpus, runs the fh1-artifact-startup
 route in an EMPTY producer state under the plugin, then packages the producer's cache via
 tools/build-fh1-gpu-prewarm.py into cache/fh1-native-shaders-v2.bin +
 cache/fh1-native-pipelines-v1.bin + cache/fh1-gpu-prewarm-v3.txt, and validates with a
 compiler-free strict run. Inputs it needs: the disc (.local/game/base, present), the producer
 sources (present), and a D3D12 adapter (RX 5700 XT present; the D3D12 backend selects adapters by
 INDEX via d3d12_adapter, unlike the D3D11 backend's adapter name).
RUN: powershell tools/produce-fh1-artifacts.ps1 -WorkRoot .local/native-renderer/production-d3d12
 -RenderTestScript config/render-tests/fh1-artifact-startup.fh1test -RuntimeConfig
 .local/d3d12-config/pinyon_shift.toml -BuildDirectory out/build/win-amd64-release -Scale 1
 -JsonEvents -AllowShaderMisses -> PRODUCE_EXIT=0, result "route-validated", 0 shader misses,
 1200 frames / 27,243 draws / 29,484 resolves, adapter "AMD Radeon RX 5700 XT".
ARTIFACTS: .local/native-renderer/production-d3d12/strict-state/cache/fh1-native-shaders-v2.bin
 (8,761,368 bytes), fh1-native-pipelines-v1.bin (1,380 bytes = 19 records), allowlist
 fh1-gpu-prewarm-v3.txt (413 bytes); D3D12 pack shaders.pnsp 24,668 entries / 44 GS / flags 09 /
 528,293,596 bytes, pack_sha256 4861C0F4D058BD3AAD587C43BE7E46ABCE4C72A241C6A7F798FAE8C2D2924104,
 content_sha256 2CAC11004DA3BBD76537999DA3301B2737B40BCFCEC263D0D1594AEB19860ECF, translator
 20260827 (production.json records all of it).
PARITY RUN: staged catalogs + D3D12 pack into .local/preview-d3d12 (config gpu_backend=d3d12) and
 ran the D3D11 capture scenario there: config/render-tests/d3d11-draws.fh1test,
 --seed-shader-storage --seed-pipeline-prewarm -> run completes 4,140 frames, but the runner
 rejects it for 29 unique "FH1 precompiled shader pack miss" [error] [gpu] lines (boot shaders the
 artifact-startup route's pack never recorded, e.g. 20A41D46F34D238E/7 - an entry the D3D11
 corpus-derived pack DOES carry, which is the one Job A's 3b check disassembles). Its capture
 (draws-frame.ppm) is uniform white (mean 255.000, one value, sha 690b027fa57a); the same scenario
 and frame on D3D11 is the frozen boot legal notice (mean 33.453, sha c1093ac9dbad). The D3D12
 notice probe (.local/probes/notice-noinput.fh1test) is white at every capture too.
VERDICT: no geometry/colour/gamma comparison is claimable. The two backends do not present the
 same scene at all - D3D11 freezes its first content frame and D3D12 presents a white frame with
 pack misses - so any MAE between the captures measures presentation state, not shading. Named
 difference that IS real: D3D12's pack is route-derived (24,668 entries, producer-validated) while
 the D3D11 pack is corpus-derived (24,437 entries); their coverage differs by design, and 29 boot
 variants present in the D3D11 pack are absent from the D3D12 one.

## JOB B — sustained-driving scenarios: premise disproved (2026-10-22)

MEASUREMENT (probes in .local/probes/, all on the current binary, GTX 570, 1x):
1. notice-noinput.fh1test (no input at all, 60 Hz wall clock): captures at 600/1200/1800/2400
   frames are ALL byte-identical (sha c1093ac9dbad, mean 33.453) and every capture event records
   game_mode 0. The title does NOT advance past the legal notice on its own.
2. notice-frames.fh1test (A tapped every 60 output frames from 300 to 1800): all 26 captures are
   byte-identical to (1), yet the run's own event log shows the title advanced - so the presses
   work and the captures do not follow.
3. fh1-long-drive (current binary): reaches game_mode 17 (free roam) by capture 1560, the wait at
   1400 on "vehicle" passes, the vehicle pose reports 30,683 updates and 2,153.01 m travelled,
   then wait 2050 fails with wait_timeout on file thumbnail_ - because the run is in FREE ROAM, not
   in car select, so no car-select thumbnail file is ever read. Not a legal-notice timing issue.
4. fh1-race-sustained (current binary): every capture (event-ready 1560 through race-sustained
   6360) records game_mode 17, and the runner still fails with "captures race-ready and
   event-step-1 differ by only 0.000 MAE (minimum 20.000)" - the scenario never enters the
   Gauntlet event, it sits in free roam. Peak VRAM during that run (nvidia-smi, 1 s poll, only
   source on this host - HWiNFO is not installed): 1,014 MB of 1,280 MiB.

ROOT CAUSE OF THE IDENTICAL CAPTURES: the presented guest output freezes on the first content
frame. During the long-drive run the guest is demonstrably in free roam and rendering - 39 FH1
surfaces created, 223,663 resolve dispatches with 0 skipped, 140k draws, 5,000+ texture uploads,
3,000 swaps presented - while every captured guest-output image, and every capture in every run
above, is the byte-identical boot legal-notice frame. RequestSwapTexture re-invalidates and
re-uploads the swap texture every swap (D3D11TextureCache::RequestSwapTexture), the presenter
paints and presents ("presented 3000 frames"), so the stale bytes are the guest-memory front
buffer the swap fetch constant points at, not a stopped paint loop. D3D11Presenter implements
CaptureGuestOutput only (no CapturePresentedOutput), so the presented-upscaler capture cannot be
used to separate "window is frozen" from "capture path is frozen" without a desktop screenshot
(a PowerShell capture was attempted twice and produced no file - not pursued further).

CONSEQUENCE: neither Job B requirement can be met by timeline authoring. The legal notice needs
exactly the presses both scenarios already make (A during 360-726 output frames), the long-drive
wait failure is a wrong-screen failure (free roam vs car select), and a capture-MAE or race-HUD
assertion cannot pass while every presented image is the boot frame. NO scenario file was edited:
there is no timeline change that produces driving evidence here. The fix belongs in the D3D11
present/resolve path (why the swap fetch's guest-memory front buffer stops receiving the resolved
frame once the title leaves the legal notice), and Gate C's stability evidence depends on it.

## JOB C — Step D wiring (2026-10-22)

C1 GAMMA FLAG (fixed). d3d11/pipeline_cache.cpp Initialize passed the host flag straight
through where the translator wants `gamma_render_target_as_unorm8`. Read both sites before the
change: d3d12/pipeline_cache.cpp:308 is `!host_config_.gamma_render_target_as_unorm16()`, so
D3D11 now passes the same inversion (the adjacent comment names the failure it caused: 8-bit
gamma render targets treated as 16-bit).

C2 PACK CONSUMPTION (wired; the only pack on disk is stale — see below).
- `D3D11CommandProcessor::InitializeShaderStorage` now overrides the base no-op and hands the
  cache root to `D3D11PipelineCache::InitializeShaderStorage`, which searches
  `cache_root/shaders/shareable` exactly like the D3D12 backend.
- `Fh1ShaderPackConfig()` = translator version, `Backend::kD3D11`, device features
  (switch statements except on Intel), flags (bindless clear, gamma-UNORM8 bit, MSAA bit),
  X/Y scale; `Fh1ShaderPack::Backend::kD3D11` + the `d3d11` pack name in
  fh1_shader_pack.cpp have DXBC-backend validation and naming.
- `TranslateShader` tries the pack first, loads its bindings through `LoadPrecompiledBindings`
  and its bytecode through `LoadPrecompiledBinary`, and falls back to runtime translation for
  genuine misses. Hit/miss/refused counters are logged at shutdown.
- Tooling: tools/native-shader-pack.py now knows the d3d11 backend (`BACKENDS["d3d11"]=3`,
  DXBC validation, its own device-feature mask), and run-fh1-render-test.py has
  `--stage-shader-pack` for a pack that must not drag in the D3D12 storage seeds
  (`--shader-pack` still requires them).
- PACK IS STALE: `.local/fh1-d3d11.pnsp` was built 2026-10-21 18:40; the SM 5.0
  dcl_constantbuffer fix in dxbc.h landed 2026-10-22 08:38. On the GTX 570 its entries are
  refused by the device at creation (E_INVALIDARG, e.g. 20A41D46F34D238E/mod 7, both of which
  are in the pack), and entries that *are* accepted remove the device on first bind - the
  pre-fix Fermi signature. A refused entry now falls back to runtime translation with a logged
  warning instead of dropping the draw; a bind-time removal cannot be probed away.
  Consequently pack use is opt-in: cvar `fh1_d3d11_shader_pack` (default false; says
  "pack found but disabled" when one is present). Regenerate the pack with the current
  translator before enabling it.
  Artifacts: d3d11-draws-20261022T171308Z (pack loaded: 24,437 entries, translator 20260827,
  features 01, flags 08, 1x1; entries refused at creation, device removed on first draw),
  d3d11-draws-20261022T171513Z (refusal fallback logs "translating it at runtime", device still
  removed by an accepted entry), d3d11-draws-20261022T171910Z (pack staged, cvar default:
  "pack found but disabled", result pass, 140,469 draws).

C3 PARITY REPORT (BLOCKED on this host). The D3D12 comparison run cannot start:
`--gpu_backend=d3d12` on this state root stops at "FH1 vertex shader ... is absent from the
offline analysis catalog" (cache/fh1-native-shaders-v2.bin and fh1-native-pipelines-v1.bin do
not exist anywhere under .local, and the D3D12 backend refuses draws without them). Building
that catalog is the disc-corpus producer pipeline, not this job. No D3D12 capture exists in
the workspace to compare against, so no shader-level parity verdict is claimed.

## JOB D — Step E (GTX 570 target pass, 2026-10-22)

Config: 1x draw resolution (the only scale the tree ships), MSAA left at the emulator default -
`native_2x_msaa=false` does NOT remove MSAA, it emulates 2x with 4x (host_render_config.cpp),
which is the opposite of the low-spec target; present_effect/fps limit are already the
LOW-SPEC 60 defaults. Host render settings unchanged beyond that.

Measured on the sustained, completing scenario (d3d11-draws, 4,140 output frames):
- d3d11-draws-20261022T174339Z (swap-wait instrumented) and jobd-gtx570-20261022T174120Z:
  result pass, draw_calls 139,091, median frame 23,941 us = 41.77 FPS median; 16 FH1 surfaces,
  152.50 MB total surface store (every surface 2.5-10 MB, listed in the run log).
- VRAM: nvidia-smi polled every second for the whole run - peak 1,051 MB of the adapter's
  1,280 MiB (1,224 MB usable), i.e. ~173 MB headroom, well under the 1,199 MB overnight
  baseline. Peak occupancy sat at 1,026 MB for most of the run.
- Swap wait (new `D3D11 presentation: wait-for-GPU ...` line, every 600 swaps): 2.43 ms mean
  at 600 swaps, 2.62 at 1,200, 2.68 at 1,800, 2.75 at 2,400 - about 11% of a 24 ms frame at
  steady state, and it is the presentation path's only CPU/GPU serialisation point.
- No device removal, no crash (result pass).

SUSTAINED-DRIVING SCENARIOS ARE BLOCKED on this build, same root cause as Gate B3: both
fh1-long-drive and fh1-race-sustained script their button presses on a 60 Hz wall clock
(frames 360-720) while this build presents the title legal notice at ~14 s and waits for input,
so the scripted presses predate the screen they dismiss. Evidence: long-drive aborts at
`wait_timeout frame 2050 file thumbnail_`; race-sustained reaches its captures but race-ready and
event-step-1 are the same frame (0.000 MAE against a 20.000 minimum). Unblocking needs a wait-
anchored press in those scenarios' timelines - a scenario-authoring decision, not made here.

## GATE RE-RUN ON THE FINAL BINARY (Job A suite, after Jobs C/D)

powershell -File .local/muse_gates_all.ps1 (final build, BUILD_EXIT=0): GATES_PASSED=4,
RUNNER_EXIT=0 and GATE_EXIT=0 on every gate (verdicts from .local/check_d3d11_gate.py, so the
log/tally conditions ran too, not just the captures):
- d3d11-cleared-frames-20261022T174607Z: pass, draw_calls 138,868, median 24,201 us = 41.32 FPS
- d3d11-texture-uploads-20261022T174720Z: pass, draw_calls 138,462, uploads 13,500, median 42.19 FPS
- d3d11-native-resolve-path-20261022T174833Z: pass, draw_calls 550,035, resolves 9,998 with 0
  skipped, median 37.29 FPS
- d3d11-draws-20261022T174907Z: pass, draw_calls 140,904, median 42.17 FPS
These supersede the 16:34-16:37Z block above as the current-binary evidence; the pack was absent
from all four (cvar off by default), so they measure the runtime-translation path.


Source of truth prompt: C:\Users\seths\Downloads\Freebuff-Pinyon-DX11-Finish-Prompt.txt
Rexglue tree: F:\PinyonShift\work\.local\rexglue (project root F:\PinyonShift\work)
No other checkout. No concurrent actor. No d3d11 lock file.

## JOB A — regression gates repaired (2026-10-22)

Scenarios changed ONLY in their capture expectations: the `# expect-cleared <name>` lines are
gone from d3d11-cleared-frames / d3d11-texture-uploads / d3d11-native-resolve-path (the
harness then applies its normal blank/degenerate guard: byte span >= 8). Timelines, inputs and
structure untouched.

Verdicts live in `.local/check_d3d11_gate.py` (new) and `.local/muse_gates_all.ps1` (now runs
all four scenarios and the checker, GATES_PASSED/GATES_FAILED in .local/muse_gates_all.log):
- cleared-frames: result pass, draw_calls > 0, present >= 30 Hz, median frame <= 40 ms,
  no 887A0005/DEVICE_REMOVED, captures non-uniform.
- texture-uploads: result pass, upload tally >= 12000, zero upload-path failures
  (CreateTexture2D/3D + SRV failures), draw_calls > 0, captures non-uniform.
- native-resolve-path: result pass, resolve tally dispatches > 0 with 0 skipped, no skipped
  plans, draw_calls > 0, captures non-uniform.
- draws: result pass, draw_calls > 0, no device removal, captures non-uniform.
Threshold notes: the pre-draw build held ~60 Hz because nothing was drawn or read back; with
the draw path and the per-swap WaitForGpu the measured median is ~40-43 Hz on the GTX 570, so
the floor asserts sustained motion (Job D quantifies the wait). The Phase 2 "17,000 uploads"
baseline is not reproducible on any artifact in the tree (14:51 build 16,000, 15:15 14,000,
final 14,500); the gate therefore floors at 12,000 and leans on zero upload-path failures.

GATE RUNS on the final binary (all four pass, GATES_PASSED=4):
- d3d11-cleared-frames-20261022T163446Z: draw_calls=152396, present 43.38 Hz, median 22661 us
- d3d11-texture-uploads-20261022T163559Z: draw_calls=149343, uploads 14500
- d3d11-native-resolve-path-20261022T163712Z: draw_calls=508574, resolves 6450, 0 skipped
- d3d11-draws-20261022T163746Z: draw_calls=141569

unit_tests side-task: ROOT CAUSE = the stale build dir, not the tree. out/build/win-amd64 has
CMAKE_CXX_COMPILER=g++ cached under the "Visual Studio 17 2022" generator, so every MSVC
feature query comes back empty (`target_compile_features ... no known features for CXX
compiler`) and the auto-reconfigure fails. A fresh configure with the project's own toolchain
(clang 20.1.8 from .local/toolchain, Ninja Multi-Config) plus the tree's documented option
REXGLUE_BUILD_TESTS=ON (OFF by default - the old dir had it enabled) configures clean and
builds the target: 868 steps, UNIT_BUILD_EXIT=0. unit_tests.exe "d3d11*" from the current
tree: All tests passed (171 assertions in 20 test cases). Commands live in
.local/muse_unit_cfg.bat / muse_unit_build3.bat. No build-config change was made.

## JOB B1 — typed mirror SRV (done)

D3D11SharedMemoryDeviceUploadSink::GetHostBufferTypedSRV() (shared_memory_upload_device.h/.cpp):
R32G32B32A32_UINT, D3D11_SRV_DIMENSION_BUFFEREX, FirstElement 0, NumElements = size/16, created
next to the raw SRV on the same mirror, logged with HRESULT + GetDeviceRemovedReason and
attempted once at host-buffer creation so the driver answer is in every run's log.
VERIFIED on the GTX 570 at FL 11_0: "typed mirror SRV (R32G32B32A32_UINT, 33554432 elements)
created hr=00000000 removed_reason=00000000" (run d3d11-native-resolve-path-20261022T164608Z).
Nothing binds it yet - creation was the deliverable.

## JOB B2 — fxc store-count sweep (recorded, not re-run)

work-tab3/.local/tab3-verify/audit_counts.tsv: 74 headers, stores at 5.1 = 149, stores at 5.0
= 149, mismatched headers = 0. CLEAN. sweep_shipped_rerun.txt holds the per-header table.

## JOB B3 — Gate B evidence (criteria met, world-content claim NOT demonstrated)

d3d11-native-resolve-path-20261022T164608Z: result pass, draw_calls=97582, resolve tally
dispatches 6450 with 0 skipped, captures non-uniform:
world-1200 min 0 max 255 mean 33.453; world-1500 same; world-1800 same - and all three PPMs are
BYTE-IDENTICAL to each other and to the title/legal-notice frame of the draws run.
So the captures prove the pipeline presents a correct frame, NOT that world geometry reaches it:
the scenario presses A at frames 360/480/600/720 (wall-clock 6/8/10/12 s), the title's
disclaimer appears later (~14 s on this build, 40 Hz presentation) and waits for input, so the
run sits on the legal notice for the remaining ~18 s. The input timeline's last button press
predates the screen it is meant to dismiss. NOT changed here (Job A forbids timeline edits and
Job B3 asks for a report): the scenario needs a later/conditional press (a `wait` step) before
"world renders" can be claimed.

## Current step (2026-10-22, Jobs 1-4)
JOB 1 — split measured with a real drain. The takeover prompt's Flush+poll pairs only
submit; a D3D11_QUERY_EVENT drain is required before polling. With per-call-site counters
(no starvation) and an event-query drain, draw #1 measured: (a) exit BindDrawRenderTargets
0 (4842 us), (b) exit ConfigurePipeline 0 (1376 us), RequestTextures 0 (116 us),
then BindPipeline's first state set 887A0005. Sub-probes inside BindPipeline convicted
VSSetShader alone (before blend/depth/rasterizer, which is why failed_call=blend was always
the cascade's first victim, not a bad descriptor).

JOB 2 — root cause and fix. The translated vertex shader is invalid for NVIDIA's Fermi
driver: OpDclConstantBuffer emitted the SM 5.0 dcl_constantbuffer operand with index
dimension 1 (register only) and the size as a trailing dword, while fxc emits a 2D operand
(register, size). Proof on a bare FL 11_0 device (GTX 570, probe at F:\PinyonShift\probe-vs):
the translator blob binds -> 887A0005; flipping only that operand field -> clean create/bind/
draw; forcing an fxc-compiled blob to dimension 1 -> 887A0005. Fix in
include/rex/graphics/format/dxbc.h OpDclConstantBuffer (SM 5.0 path only). Two clean
`d3d11-draws` runs after the fix: 205,885 binds and zero 887A0005.

JOB 3 — scaffolding out + SRV fix. All .musebak6-.musebak14 diagnostics removed; the three
stubs reverted (surface clears, guest draw issuance, texture upload copy); only the .musebak5
HRESULT logging kept. Mirror SRV fixed to BUFFEREX + RAW (was E_INVALIDARG on every call).

JOB 4 — gates (build BUILD_EXIT=0, 0 errors):
- d3d11-cleared-frames: PASS (result.json, 4144 samples, 70.01 s, median 59.96 FPS).
- d3d11-texture-uploads: PASS (result.json).
- d3d11-draws: capture_failed "blank" — draws DO issue (51 draws / 11,098 vertices in the
  final frame, 0 device removals, 0 GPU errors) but IssueSwap clears the guest output black,
  so the harness's non-blank capture check aborts before result.json. Blocked on the
  presentation path (Step B), which this prompt keeps out of scope.
- d3d11-native-resolve-path: PASS (3 cleared captures, 26,049 resolve dispatches, 0 skipped).
Also wired PROFILE_DRAW_CALL/PROFILE_VERTICES in the D3D11 IssueDraw (draw_calls was never
reported on this backend, so the gate's metric read 0 by construction).

RE-RUN ON THE FINAL BINARY (post-counter build), all three legacy gates:
- cleared-frames  (d3d11-cleared-frames-20261022T151444Z): result=pass, draw_calls=181498
- texture-uploads (d3d11-texture-uploads-20261022T151556Z): result=pass, draw_calls=181135
- draws           (d3d11-draws-20261022T151709Z): draw_calls=181498 (>=60k criterion met),
  0 device removals, 0 GPU errors, capture 2764816 bytes but blank -> harness aborts.
RESOLVED (user chose "wire the frontbuffer refresher now"): the D3D11 backend presents the
resolved frontbuffer. Gate A now PASSES.

PRESENTATION PATH (final form):
- fh1 texture cache `RequestSwapTexture` (applied): texture fetch 0 -> TextureKey -> cache
  texture; before loading, `shared_memory().MemoryInvalidationCallback(base, base_size, true)`
  clears the GPU-written flags the resolve sets. Without it CopyCpuRange refuses every
  resolved page and the cached swap texture freezes at its first (black) upload - that was the
  entire remaining blank-capture cause.
- signal: `D3D11CommandProcessor::WaitForGpu()` (D3D11_QUERY_EVENT, spin on GetData) before the
  request - no submission fences on this backend.
- blit: `D3D11Presenter::PresentGuestOutputTexture` reuses the paint path's rect VS + bilinear
  PS + sampler + blend/depth/rasterizer states (a null rasterizer state culls the quad: that
  silently black-screens it). Falls back to the black clear when there is no frontbuffer.
- `IssueSwap` reports `NativeGuestOutputPresenter::kNativeExecutor` and AddRef/Releases the swap
  SRV across the refresh.

GATES on the final binary (d3d11-draws-20261022T162438Z):
- d3d11-draws: PASS (result.json, draw_calls=140393, real title frame captured).
- d3d11-cleared-frames / d3d11-texture-uploads / d3d11-native-resolve-path: FAIL *by
  expectation* - all three assert "expected a cleared (uniform) image" and the capture is now
  the actual FH1 legal-notice frame (byte span 0-255, 127 distinct values). Their premise (a
  black presented frame) is what this change removed; the scenarios need their clearance
  assertions retired or replaced with content assertions.
Evidence image: .local/draws_check.png (1280x720, the captured guest output).


## Previous step
STEP A (Phase 3, gate: draws issue) — pipeline_cache.cpp `->state->`/`state.get()` fixes applied and verified. Release build clean (0 errors, only pre-existing warnings). All `state` member accesses corrected: (1) `->state->`/`->state.get()` to direct `.` access for struct members, (2) `it->second.get()` to `it->second->state.get()` for proper unique_ptr dereference. Build+verdict: clean. A5 D3D11 unit tests: ALL PASSED (20 test cases, 171 assertions) — `unit_tests.exe "d3d11*"` from `out/win-amd64/Release`. Gates to re-run fresh (GTX 570): Phase 1 `d3d11-cleared-frames`, Phase 2 `d3d11-texture-uploads` (old results not cited). Gate A `d3d11-draws` (>=60s) pending execution/report.

## Gate verdicts recorded so far
- Phase 1 gate: PASS — "d3d11-cleared-frames" (4,165 frames / 70 s, GTX 570 FL11_0, median 59.98 FPS).
- Phase 2 gate: PASS — "d3d11-texture-uploads" (4,122 frames / 70 s, 17000 textures / 6024290 KB, 0 failures).
- "d3d11-native-resolve-path" probe family: ran 4x today (17:51–18:08 UTC). Verdict of NEWEST run
  = Phase 4 entry baseline (to be read at Step B1; not yet read at Step A).
- GATE A/B/C/D/E: not run.

## True file inventory (verified 2026-10-21 from disk + git status)
include/rex/graphics/d3d11/: command_processor.h (3025), fh1_native_resolve.h (2888),
  fh1_native_surfaces.h (4115), graphics_system.h (1569), host_render_config.h (3404),
  pipeline_cache.h (10653), primitive_processor.h (1631, NO-OP SKELETON), render_target_cache.h (2210),
  shared_memory.h (1590), shared_memory_upload.h (2323), shared_memory_upload_device.h (2543),
  shader.h (2991), texture_cache.h (6544).
src/graphics/d3d11/: command_processor.cpp (15454; IssueCopy works, IssueSwap wired, IssueDraw drops
  draws), fh1_native_resolve.cpp (9259), fh1_native_surfaces.cpp (7768), graphics_system.cpp (1288),
  host_render_config.cpp (7986), pipeline_cache.cpp (32843),
  primitive_processor.cpp (1501, NO-OP SKELETON), render_target_cache.cpp (1720),
  shared_memory.cpp (2739), shared_memory_upload_device.cpp (6543),
  shader.cpp (2574), texture_cache.cpp (42470, COMPLETE incl. GetActiveTextureBindfulSRV / GetSamplerDesc).
  Present: shader.h, pipeline_cache.h, shader.cpp, pipeline_cache.cpp.
Untracked additions relevant to port: src/graphics/pipeline/shader/dxbc_geometry_shaders.{h,cpp}
  (shared GS DXBC builder, "Shared by the DXBC-based backends (D3D11 and D3D12)"),
  src/graphics/shaders/fullscreen_cw.vs.hlsl, src/graphics/shaders/bytecode/d3d11_5_0/,
  src/ui/shaders/d3d11/, tests/unit/graphics/d3d11_*_test.cpp.
Modified tracked files (from earlier phases): src/graphics/CMakeLists.txt, conversion.{h,cpp},
  d3d12/pipeline_cache.{h,cpp} (shared-code changes; must stay backend-conditional), compile-fh1-native.sh.

## Key design facts established for the port (from disk)
- DxbcShaderTranslator(vendor_id, bindless_resources_used, edram_rov_used, ...) — D3D11 uses
  bindless_resources_used=false -> flat registers: b0=system constants, b1=float constants,
  b2=bool/loop, b3=fetch constants; t0=shared memory mirror ByteAddressBuffer, t1+=bindful textures
  (bindful_srv_index); s#=samplers (bindful index). (dxbc_translator.h SRVSpace/SRVMainRegister/
  CbufferRegister enums.)
- Shared-memory host buffer already has BindFlags SRV|UAV|INDEX_BUFFER (Phase 3 prepared).
- D3D12 templates for shape/semantics: src/graphics/d3d12/{shader.cpp, pipeline_cache.cpp,
  primitive_processor.cpp, command_processor.cpp, fh1_native_executor.cpp}.
- PrimitiveProcessor base contract: InitializeCommon(true, false, false, true, true, true) like D3D12
  (D3D11 FL11_0: full 32-bit indices; no fans/loops; quads->lists via GS; point/rect via translator).
- geometry_shaders: GeometryShaderKey + CreateDxbcGeometryShader() in
  include/rex/graphics/pipeline/shader/dxbc_geometry_shaders.h (identical DXBC for D3D11).
- Guest shader blobs: translator stamps SM 5.1; FL 11_0 rejects -> Gate A logs+counts rejected
  guest shader hashes; SM 5.0 retarget is Step D.

## Step log
- Step A0: re-verified inventory via git status + ls (2026-10-21 ~13:50 MDT). Todo list reset to
  prompt steps A-E.
