# Phase 3 M3 — Draw recording through the command tape (result)

Date: 2026-10-26
Tree: `F:\PinyonShift\work` (live DX11 tree, read-only for this phase's output)
Binary: `.local\muse_build_game.bat`, `BUILD_EXIT=0`
GPU: GTX 570 by name, monitor 2 only (monitor 1 untouched)
Flags in every run: `REX_FH1_D3D11_SHADER_PACK=true`, `REX_FH1_NATIVE_EXECUTOR=1`,
`REX_FH1_D3D11_RESOLVE_READBACK=1`, `REX_FH1_RESOLVE_GUEST_REFRESH=1`; the tape is
`REX_FH1_D3D11_COMMAND_LISTS=1`. **No default changed**: `fh1_native_executor`,
`fh1_d3d11_resolve_readback`, `fh1_resolve_guest_refresh`, `fh1_d3d11_command_lists`
all keep their previous defaults.

Status: **implemented and measured; two plan acceptance items are not measurable
as written and are reported with substitutes below (see "Acceptance").**

## 1. What was built

| Piece | Where | What it does |
|---|---|---|
| `D3D11DrawPacket` | `include/rex/graphics/d3d11/command_list.h:194` | Shaders, GS, blend/depth/rasterizer state, five constant-buffer blobs, 4+4 bound CBs per stage, 33 SRVs per stage, 16 samplers per stage, 8 RTVs + DSV, viewport, scissor, blend factor, sample mask, stencil ref, topology, index buffer/format/offset, vertex count, indexed flag. |
| `RecordDrawPacket` | `src/graphics/d3d11/command_processor.cpp:2910` | Reads the state the draw just bound back from the immediate context (not a second description of intent) and appends it with the draw arguments. Returns false when the packet cannot hold the draw; the caller then issues it immediately. |
| constant blobs at record time | `command_processor.cpp:1120-1160`, `command_list_immediate_sink.cpp:244` | `UpdateConstantBuffer` keeps the exact bytes of the record-time update in `ConstantBufferBinding::cpu_bytes` and, while a draw is being recorded, does **not** issue `UpdateSubresource`. The packet carries a padded copy; replay uploads it before the draw. |
| index ring captured at record time | `primitive_processor.cpp:108` (+ `RecordDrawPacket`) | Converted indices stay in the per-frame DYNAMIC ring page the record path mapped; the packet holds the page (`ComPtr`) and the byte offset. Guest-DMA indices stay bound straight from the shared-memory mirror. Pages are not reused within a frame, so record-time bytes are replay-time bytes. |
| texture uploads on the tape | `command_processor.cpp:2840`, `texture_cache.cpp:688` | The cache keeps its CPU untiling at record time, appends an `UploadTexture` packet and skips its own `UpdateSubresource`; a failed append falls back to the inline upload. |
| replay sink | `command_list_immediate_sink.cpp:259` | Rebinds every recorded binding and issues one `Draw`/`DrawIndexed`; never relies on state left by an earlier packet. |
| drains | `command_processor.cpp:2796`, `2831` | `SubmitFrameTape` replays and resets the tape; `DrainTapeForDiagnostic` is called before every diagnostic GPU read (texture dump, surface dump, resolve source probe) and at target prep, so a diagnostic never samples an undrained tape. |
| counters | `command_processor.cpp:548` (`LogTapeCounters`) | Submissions, packets, arena bytes, resolve/draw/upload packets, and draws left immediate (fallback / unrecordable / empty viewport). Periodic (every 64 swaps) and at shutdown, because the title hard-exits. |
| validation | `command_list_immediate_sink.cpp:386` | With `fh1_d3d11_command_list_validate=1`, reads the just-bound state back and compares it with the packet: shaders, 4+4 CBs, SRVs, samplers, RTVs + DSV, **viewport and scissor** (added this milestone). |

## 2. Defects found and fixed during M3

1. **The constant block did not reach the GPU.** The first replay path used a boxed
   `UpdateSubresource` (468-byte block). Measured: `gpu_first=00000000,00000000` against
   `blob_first=00000258,40000000` — the boxed form did not land on this driver for a block
   whose size is not a multiple of 16, so every replayed draw read zeros. Fixed by padding
   each blob to the buffer's real ByteWidth and issuing the same full-buffer
   `UpdateSubresource` the immediate path issues. After the fix:
   `FH1 draw tape CB check: draw=205 size=480 gpu_first=00000258,40000000 blob_first=00000258,40000000`.
2. **A draw with no viewport would have replayed with a zero-sized one.** `RSGetViewports`
   returns 0 when nothing was set and Direct3D 11 then uses the default full-target
   viewport; recording that as a 1-viewport 0×0 would have silently dropped the geometry.
   Such a draw is now left immediate and counted (`empty_viewport`). Measured count: **0**
   in the runs below, i.e. the guard never fired — it is a guard, not a workaround.

## 3. Measurements

### 3.1 Regression on defaults

| Check | Result |
|---|---|
| Build | `BUILD_EXIT=0` (`.local/diag/p3m3-build2.log`) |
| trace-short, defaults, `--hidden` | `t0300.ppm` **b8f68e89c6192577747ab452aa5598ef**, `t0600.ppm` **b8f68e89c6192577747ab452aa5598ef** — the recorded baseline |
| trace-short with `REX_FH1_D3D11_COMMAND_LISTS=1` | same captures, byte-identical: both **b8f68e89c6192577747ab452aa5598ef** (one scan-deterministic scenario where off and on can be compared exactly) |
| Tape with the flag off | zero `D3D11 command lists` lines in the trace-short log: no packet, sink or tracker is constructed (the `--hidden` trace profile does not preserve a per-run log, so the trace-short ON run is evidenced by its captures; the roam runs below carry the packet counts) |
| Device-free tests, `d3d11*` filter | 20 test cases, 171 assertions, all pass (the sink and the processor are not linked into that target; the tape/packet code is unchanged since that build) |

### 3.2 Validation run (tape on + validate on, free roam to output frame 5460)

```
D3D11 draw tape validation: draws=499200 failures=0 last=0 indexed=1 verts=372 srvs=1/1 samplers=0/0 targets=1 dsv=0
D3D11 draw tape validation: draws=498688 failures=0 last=0 indexed=1 verts=124 srvs=1/3 samplers=0/1 targets=1 dsv=1
```

975 summaries, **0 failures**, 499,200 replayed draws checked binding-for-binding
(shaders, CBs, SRVs, samplers, RTV/DSV, viewport, scissor). 0 mismatch warnings.

Tape accounting, same run (last periodic line before the stop):

```
D3D11 command lists: 473120 submissions, 634414 packets, 14909590224 arena bytes;
20821 resolve packets, 430681 draw packets, 37031 texture uploads;
draws immediate[fallback=0 unrecordable=0 empty_viewport=0]
```

- **0 draws left the tape** for any reason (no packet cap, no SRV/sampler overflow, no
  missing index buffer, no overflow retry).
- 14.9 GB of arena over 395 swaps ≈ 37 MB/swap, dominated by texture uploads
  (37,031 uploads) and the five constant blocks per draw.
- Replayed draws (499,200 at the stop) exceed the last periodic packet count (430,681)
  because the counter line is periodic, not final: the title hard-exits before the
  shutdown summary.

### 3.3 Draw counters, off vs on

The D3D11 per-frame counter line is emitted on both paths and every field is **0** —
`passes=0 drawn=0 dropped_all=0 other=0` — because this title's draws go through the FH1
native executor, which does not use `draw_counters_`. Summed over the run:

| | lines | passes | drawn | dropped_all | other |
|---|---|---|---|---|---|
| off | 395 | 0 | 0 | 0 | 0 |
| on | 394 | 0 | 0 | 0 | 0 |

Counters are therefore identical (both empty), and the meaningful draw accounting for this
title is the tape's: 430,681 recorded draws, 0 left immediate, 499,200 replayed and
validated.

### 3.4 The bound t5 texture at a world swap, off vs on

Same probe (`roam-texdump.fh1test`), same window, `REX_FH1_DEBUG_TEXTURE_DUMP`,
`REX_FH1_DEBUG_COMPOSITE_DRAWS=300`, `REX_FH1_DEBUG_COMPOSITE_WORLD_ONLY=1`.
48 dumps are common to both runs (swaps 303–350), each the 640×360 texture at
`1CA95000` (`srv_fmt=28`, fetch swizzle `0x60A`):

| metric | off (48 dumps) min / mean / max | on (48 dumps) min / mean / max | Phase 12 baseline |
|---|---|---|---|
| share of the single word `0x00007F7F` | 54.22 / **63.36** / 73.45 % | 57.65 / **64.63** / 74.56 % | 60 % |
| share of texels with B==0 | 77.40 / **84.04** / 90.16 % | 73.11 / **82.85** / 88.82 % | 85 % |
| distinct RGB triples | 10,874 / **13,926** / 15,828 | 11,517 / **15,858** / 19,329 | 17,889 |
| mean RGB | 131.5 / 128.3 / 25.6 | 133.3 / 131.1 / 27.8 | — |

Matched swaps: `world-303-t5` off 71.15 % word, 81.91 % B==0, 12,257 triples,
mean 132.4/128.4/31.0 — on 72.33 %, 80.26 %, 15,784 triples, mean 130.8/131.6/35.4.
`world-350-t5` off 54.22 %, 77.56 %, 13,379 — on 58.53 %, 73.33 %, 14,179.

Both paths sit in the Phase 12 poisoned family, as required while the Phase 13 fix stays
unflipped: **M3 reproduces the baseline, it does not improve or cure it.**

### 3.5 Why on/off pixel identity is not measurable for this title

The plan asks for identical frame/t5 dumps and "no visual difference" between on and off.
That comparison is not valid here: the guest simulation advances on wall-clock time, so
two runs do not present the same scene at the same output frame. Measured on the same
binary and probe:

| run | 3600 | 4200 | 4800 | 5400 | (mean RGB at 3600) |
|---|---|---|---|---|---|
| off  | 70,540 colours | 25,522 | 23,784 | 20,220 | 87.3 / 105.6 / 206.8 |
| off2 | 22,611 colours | 6,045 | 6,371 | 7,178 | 62.9 / 69.5 / 200.1 |
| on   | 103,904 colours | 43,454 | 22,268 | 1 | 109.5 / 122.3 / 124.4 |
| on2  | 34,634 colours | 6,500 | 6,148 | 4,627 | 98.4 / 60.8 / 102.0 |

Off vs off differs by 3.1× in colour count at frame 3600; on vs on differs by 3.0×. A
cross-run pixel test would therefore fail on the immediate path too. What *is* measured
above is: (a) same-swap t5 metrics in the same family (3.4), (b) every replayed draw
rebinding exactly what the packet recorded (3.2), and (c) the CB bytes the GPU holds after
replay equal to the blob (2.1).

**Open observation, not attributed:** the first ON run ended with a fully black presented
frame at output frame 5400 (1 colour, mean 0/0/0) while its front-buffer readback at swap
394 carried `readback_words_nz=28887` with `readback_first=C0A00C03`. A second ON run on
the same binary presented content at 5400 (4,627 colours). Recorded for M4, whose subject
is exactly the swap list / readback / fence ordering.

## 4. Acceptance against the plan

| Plan item | Result |
|---|---|
| Constant blobs captured at record time | Yes — packet carries them, replay uploads them; GPU readback equals the blob (`gpu_first=00000258,40000000`) |
| Index ring captured at record time | Yes — page + offset, pages not reused within a frame; guest indices bind the mirror directly |
| Texture uploads ordered with the draw | Yes — `UploadTexture` on the tape, inline fallback on append failure; 37,031 uploads recorded, 0 unresolved |
| Dumps match the immediate path | Same family, not bit-identical; cross-run identity is not measurable (§3.5) |
| Draw counters identical | Identical (both report 0 on this title); tape: 430,681 recorded, 0 left immediate |
| No draw with unbound CB/index buffer | 0 validation failures over 499,200 draws |
| No visual difference, on vs off | Not measurable across runs (§3.5); within-run state identity proven by validation |
| Flag-off trace-short unchanged | Yes, md5 `b8f68e89c6192577747ab452aa5598ef` |

## 5. Left for M4

1. Swap list, the front-buffer readback fence, and the one observed black presented frame
   (§3.5) — that is M4's subject.
2. A *within-run* pixel A/B for draws (replay vs immediate on a duplicated target) if a
   pixel-level proof beyond state validation is wanted; it needs a target readback and a
   restore, and it is not in this phase's plan.
3. Performance of the current shape: 473k submissions / 395 swaps (≈1,200 per swap, ~1.3
   packets each) is a packet per draw plus a resolve-boundary drain; M6 owns the numbers.

## 6. Reproduction

```
set REX_FH1_D3D11_SHADER_PACK=true
set REX_FH1_NATIVE_EXECUTOR=1
set REX_FH1_D3D11_RESOLVE_READBACK=1
set REX_FH1_RESOLVE_GUEST_REFRESH=1
set REX_FH1_D3D11_COMMAND_LISTS=1
set REX_FH1_D3D11_COMMAND_LIST_VALIDATE=1
set REX_FH1_SWAP_FETCH_TRACE=true
py -3.11 tools\run-fh1-render-test.py .local\probes\roam-texdump.fh1test ^
  --state-root .local\diag\p3m0-off.state --output .local\diag\p3m3-val --timeout 1200
```

Artifacts: `.local/diag/p3m3-val.state/logs/runtime.log` (validation + tape counters),
`.local/diag/p3m3-roam2-{off,on,off2,on2}` (capture sets), `.local/diag/m3tex2-{off,on}`
(t5 dumps and `c2-after-*` surfaces), `.local/diag/p3m3-trace` (trace-short md5).
