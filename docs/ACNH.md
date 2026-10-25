# Animal Crossing: New Horizons: what we know about the game

Facts measured on our own dump: base 1.0.0 (`bd68bf29….nca`, SDK 9.3.1) plus the 3.0.2 update (`fd0780de….nca`,
SDK 20.5.17, version `0x210000` = 2162688). The 3.0.3 update is supported too (see [Updates](#updates)).
Everything here can be reproduced with the tools named next to it.
Derived files stay in `private/acnh/` (gitignored).

## Title

| | |
|---|---|
| Program id | `01006F8002326000` (update `01006F8002326800`) |
| Version targeted | **3.0.2** or **3.0.3** (base + update). The update's ExeFS is complete; its RomFS is a BKTR patch over the base's |
| main.npdm | main thread priority 44, core 0, stack 1 MiB, **no system resource** (META +0x14 = 0) |
| Engine | Nintendo EPD's in-house engine, called **Park** in file names (`Park_UBER_All`, `Park_Idr`) |

## Updates

Two updates are supported. Offsets everywhere else in the docs and the code are 3.0.2's unless marked.

| | 3.0.2 | 3.0.3 |
|---|---|---|
| Title version | 2162688 (release 33) | 2228224 (release 34) |
| main build id | `FCD2BB238ABE99E925B4E452DF1F41F1` | `FF1D1C05670DB6021C85B624A710B963` |
| sdk | SDK 20.5 | SDK 20.5.4 |
| Hooked instructions | `games/acnh/hooks.txt` | `games/acnh/hooks_303.txt` |

**What moved.** 3.0.3 is a small fix: of the 6,417 code and data words tmouh's SpeedFix patches, 6,375 are the same in
its 3.0.2 and 3.0.3 versions and the rest are PC-relative branches. Pairing the two patches site by site gives two
shifts only: none up to main+25bfe88 and from main+4002e40, **+0x10** from main+280c154 to main+3c88108. The edges lie
in the gaps between patch sites; the hooks past main+3c88108 (main+3dc3298, main+3e01f08) were checked by hand and
moved too, so the range runs to at least main+3e01f18. Read-only data did not move (the projection vtables
main+4a65608 / main+4a65688 list the moved doUpdateMatrix entries, main+37e5968 / main+37e5b6c; the memory budget
table at main+404d658 holds the same values).

**What the runtime uses that moved**, every one checked in the 3.0.3 disassembly (the instruction its comment
describes, or a function entry right after a `ret`):

| Use | 3.0.2 | 3.0.3 |
|---|---|---|
| Hooks (`hooks.txt` vs `hooks_303.txt`; `NX_HOOK` registers both, the unused one is never called) | main+38b2b34, 3a09404, 3e01f08, 3dc3298, 3a1d614, 37e5b5c, 37e5958, 31c3f98, 3c48c60, 3c4b1fc, 37e4624 | +0x10 each |
| Same in both | main+27d4c0, 6d659c, 2216938, 226af00, 7c6430 | unchanged |
| Clear on bind call sites (`NX_SKIP_CLEAR_AT`, `NX_CLEAR_ONCE_AT`, `games/acnh/game.cpp`) | main+37fb4fc, 37fb744 | main+37fb50c, 37fb754 |
| Focus manager SetFocus (`games/acnh/pointer.cpp`) | main+3c48b68 | main+3c48b78 |

With the 3.0.2 clear sites on 3.0.3 every frame is black (the clears they skip wipe the HDR target), which is how a
missed offset shows. At run time `acnh::is_303()` (`games/acnh/version.h`) tells the two apart by the colour clear
call site's four words at main+37fb500; `setup_acnh.sh` and `setup_60fps.sh` pick the hook list and the SpeedFix patch
(`speedfix_main_302.pchtxt` or, for 3.0.3, `speedfix_main.pchtxt`) by main's build id.

## Modules (`tools/probe.py private/acnh/exefs`)

| Module | Size | Imports / exports | What | Base used |
|---|---|---|---|---|
| `main` | 0x4b57930 (75 MiB) | 1,015 / 52 | the game, Havok Animation + Cloth 2019.1, NEX 4.6.11, Pia 5.31.5, NintendoWare Atk/Font/G3d/Ui2d 9.3.1 and Vfx2 20.5.4, iWnn 1.9.0 (Omron's IME), libcurl, zlib | `0x40_0000_0000` |
| `sdk` | 0xb834e8 | 8 / 23,025 | nnSdk 20.5.4 with NVN 20.5.4 | `0x40_1000_0000` |
| `subsdk0` | 0x552b88 | 663 / 9,446 | `NintendoSDK_movie` 20.5.4 (`nn::movie`) | `0x40_1800_0000` |

`subsdk0` is new compared with Wonder: main imports `nn::movie` from it, so it is recompiled as a third module
(`NX_HAS_SUBSDK0`, `runtime/src/main.cpp`). The three bases fit the runtime's 512 MiB module reservation.

Imports of main + subsdk0 per service: libc 389, c++ 375, nn::audio 186, nn::os 103, nn::hid 60, nn::socket 38,
nn::util 33, nn::fs 32, nn::time 25, nn::ldn 25, nn::fontll 24, nn::prepo 23, nn::account 23, nn::oe 22,
nn::crypto 22, nn::friends 16, nn::bcat 16, nn::sf 13, nn::image 12, nn::nfp 12 (amiibo), nn::nifm 11, ...

## Recompilation (`python -m recomp`)

| Module | Functions | Jump tables | Instructions | Time | Generated |
|---|---|---|---|---|---|
| main | 178,127 | 4,190 | 16,445,201 | ~7 min, ~3 GB RAM | 1,321 files, 1.4 GB |
| sdk | 40,491 | 610 | 1,674,143 | 40 s | 138 files |
| subsdk0 | 13,954 | 334 | 900,179 | 20 s | 73 files |

All with **0 unhandled instructions**. Instructions added for SDK 20 / ACNH (`recomp/emit_sdk20.py`), each checked
against Unicorn with `tests/difftest.py --filter ...` (0 failing shapes):

- `sqshl` (vector immediate, scalar and vector register), `sqrshl`, `uqshl`, `sqabs`, `sqneg`, `saddlv`
- `sli`, `sri` (shift and insert), `pmull`/`pmull2` on bytes (8B to 8H), `tbx` (table lookup that keeps lanes)
- `bfc`, `umnegl`, `smnegl`
- `csdb` (speculation barrier) and MTE's `stg` as no-ops

Analysis fix found on ACNH (it applies to every game): functions that share a tail share its jump tables, and a
table can reach back before a later entry (`main` 0x3aeaf14 runs into `br` 0x3aeb000, whose case 0 is 0x3aeaf0c,
in the function before). Those targets are now promoted to entries and tail-called (`recomp/analysis.py`,
`recomp/emit.py` `i_br`). Without it the C++ did not compile (`goto` to a label of another function); the `sdk`
had 6 such targets too.

## RomFS (3.0.2 applied)

103,400 files, 242 folders, 10.5 GB. Compression is **zstd** (`.zs`, 61k files) and **Yaz0** (`.szs`):
`tools/maxwell/yaz0.py` (with a small C decoder built on first use) handles Yaz0, including Yaz0 members nested in
SARC archives.

## Shaders (`tools/extract_shaders.sh`, `tools/maxwell/containers.py`)

| Where | Container chain | Notes |
|---|---|---|
| `Shader/Park_UBER_All.Nin_NX_NVN.szs` | Yaz0 → SARC → FSHA → BNSH | 6,520 programs, the main material shader |
| `Shader/Park_Proc.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BAHS (agl) | |
| `Shader/apkg.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BNSH | |
| `Effect/Park_{Idr,Static}.Nin_NX_NVN.szs` | Yaz0 → SARC → VFXB (`0x26`) → **GRSN / GRSC → FSHA** → BNSH | VFXB sections hold a shader *archive* here, not a bare BNSH as in Wonder |
| `System/GSys/gsys_resource.Nin_NX_NVN.release.szs` | Yaz0 → SARC → BAHS | |
| `Swkbd/shader/*_Nx.arc.szs` | Yaz0 → SARC → BNSH | the game's own software keyboard |
| `Layout/*.Nin_NX_NVN.zs` | zstd → SARC → BNSH (`bgsh`) | 934 of 960 layouts |
| `main` | raw blocks (`tools/maxwell/exefs_shaders.py`) | 35 |

**SDK 20 compute shaders have no NVN header**: the code block starts straight with Maxwell code (the control
section's program start, +0x708, is 0), where SDK 16 had the `0x12345679` header and a start of 0x100. The
container check and `spirv_harness` recognise both. A few **geometry** stages exist (16 stages, 4 blocks).

Result of `spirv_harness --batch`: **6,200 distinct blocks (23,455 stages), 6,172 ok, 28 ok-retry, 0 invalid,
0 exceptions**, validated with `spirv-val` (vulkan1.2), in about 70 s on 6 jobs.

## Game-specific notes

- **Keyboard:** ACNH ships its own software keyboard (`Swkbd/`: iWnn dictionaries `NJDC`/`NJAD`, layouts, shaders),
  so naming the island and the player does not go through the system's swkbd applet.
- **Clock:** the game's day and season come from `nn::time` (the user system clock). The runtime answers it from the
  host clock (`runtime/src/svc_time.cpp`), so the island follows the computer's date and time.
- **Online:** NEX/Pia/friends/bcat/nifm are answered offline (no network connection), as in Hades and Wonder.
- **Touch / mouse:** the left mouse button over the picture is the touch screen (`runtime/src/hle_hid.cpp`), and a
  click is answered by the game's two UI systems (`games/acnh/pointer.cpp`):
  - `guiTouchManager::update()` (main+31c3b50), the game's own touch support, reads `GetTouchScreenStates<16>`: a
    touch grabs the registered button under it (hit test main+31c3360 over two layers, the second one for windows)
    and presses it when the touch ends with a sample carrying the End attribute (a touch that just disappears
    cancels). It moves every touch 15 up (fingertip), which `touch_y_bias` undoes. Registered: the software keyboard
    and the buttons of windows such as "Are you OK with ...?", which the game leaves out of reach (their window's
    content node is disabled unless it scrolls); a hook before its hit tests (main+31c3f98) enables them.
  - The layout framework's focus managers (main+3c48c60, one per page with a cursor: dialogue choices, the birthday
    and character screens, menus) know no touch. A hook on their input (main+3c48c60) hit-tests the click against
    their items' rectangles (target+0x14a0 centre, +0x14a8 size, layout units of 1920x1080 around the centre),
    moves the cursor there as the game does (main+31017d0, main+3c48b68) and presses A for three frames. A page
    whose cursor is not shown yet (all items disabled, the intro's style choice) gets an A first to show it. Clicks
    the touch tree took are left alone. Moving the mouse (button up) over an item moves the cursor there without
    A; a mouse that does not move leaves the cursor to the pad.
  - The message window: its page turner (class main+0x49fc8f0) has button handlers (main+3c4b1fc; A and B) that
    are on only while a page is fully shown and waits (the ▼). A click then, one no list item took, presses A
    (hook on main+3c4b1fc). With no message waiting, a click in the world does nothing.
  The controller's own touch pointer (main+2aeeefc, written to its input record +0x24/+0x28) is never read, and a
  pointer handler of the layout framework (main+3c4be98) exists but nothing registers one. NX_TRACE_TOUCH=1 logs
  touches, the items clicked or hovered and the ones a click missed; NX_INPUT `<frame>:M<x>x<y>` moves the mouse.
