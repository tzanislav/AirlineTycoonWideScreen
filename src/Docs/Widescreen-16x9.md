# Widescreen (16:9) Conversion Plan

Goal: render the game at 16:9 (e.g. 854×480 internal resolution) instead of
640×480. **Phase 1 targets the airport view only**; rooms remain 4:3 (their
backgrounds are pre-rendered 640×480 images) and are shown centered with
side bars until their art is reworked.

---

## 1. How rendering works today

| Step | Where | What |
|---|---|---|
| Window + back buffer created | `GameFrame.cpp` (~L300–330) | `PrimaryBm.ReSize(h, bFullscreen, XY(640, 480))` |
| Back buffer texture | `SBLib/source/CBitmap.cpp` `SB_CPrimaryBitmap::Create` | Streaming 640×480 RGB565 texture (or software surface) |
| Viewport (4:3 letterbox) | `GameFrame.cpp` `UpdateFrameSize()` | `SetTarget()` computes a centered 4:3 rect via `getAspectWidth()` |
| Present | `CBitmap.cpp` `SB_CPrimaryBitmap::Present()` | `SDL_RenderCopy` texture → target rect |
| Mouse mapping | `GameFrame.cpp` `TranslatePointToGameSpace` / `TranslatePointToScreenSpace` | Window coords ↔ hardcoded 640×480 |
| Airport view | `AirportView.cpp` | Scrolling world; visible width hardcoded as 640, half-width as 320 |
| Status bar | `StdRaum.cpp` (~L369, L2740–2800) | 40 px tall, starts at y=440, positioned partly via `WinP1/WinP2`, partly hardcoded |

The resolution is **not centralized**: ~38 files contain literal `640`/`480`,
plus derived literals (`320`, `440`, `630`, `639`). Heaviest: `Editor.cpp`,
`StdRaum.cpp`, `GameFrame.cpp`, `AirportView.cpp`, `Sbbm.cpp`, `ColorFx.cpp`.

---

## 2. Choose the target resolution

Keep the height at **480** so all existing art, fonts and vertical layout stay
valid. Only the width grows.

- 16:9 at 480 tall = 853.33 → use **854** (or **848/856** if a multiple of 8
  is preferred for blitting/RLE code — check `Sbbm.cpp`/`ColorFx.cpp` for
  alignment assumptions).
- Extra width: 214 px → **107 px per side** when centering 4:3 content.

---

## 3. Step 1 — Centralize screen dimensions (no behavior change)

Introduce constants and replace literals before changing any size. This is
the bulk of the mechanical work and can be done and tested at 640 first.

```cpp
// defines.h (or a new Screen.h)
constexpr SLONG kScreenW      = 640;   // later: 854
constexpr SLONG kScreenH      = 480;
constexpr SLONG kLegacyW      = 640;   // width of 4:3 room art
constexpr SLONG kRoomOffsetX  = (kScreenW - kLegacyW) / 2; // pillarbox offset for rooms
constexpr SLONG kStatusBarY   = 440;   // kScreenH - StatusLineSizeY
```

Replacement rules — **do not search/replace blindly**; classify each hit:

| Literal | Means | Replace with |
|---|---|---|
| `640` as screen width/clip | screen | `kScreenW` |
| `640` as room art width | legacy | `kLegacyW` |
| `320` as view center / half width | screen | `kScreenW / 2` |
| `630`, `639`, `634` near right edge | screen | `kScreenW - 10`, `kScreenW - 1`, … |
| `440` | status bar top | `kStatusBarY` |
| `640` used as a distance threshold (e.g. `abs(dx) > 640`) | gameplay | usually `kScreenW` (means "off-screen") |

Verify the game is pixel-identical at 640 after this step.

---

## 4. Step 2 — Back buffer and viewport

1. **`GameFrame.cpp`**: `PrimaryBm.ReSize(..., XY(kScreenW, kScreenH))` and
   `ReSizePartB` likewise.
2. **`getAspectWidth()`**: use `kScreenW / kScreenH` instead of `640/480`.
3. **`UpdateFrameSize()`**: with `OptionKeepAspectRatio == 1`, the target
   rect is now 16:9. Also handle windows *narrower* than 16:9 (letterbox
   vertically) — current code only pillarboxes.
4. **`TranslatePointToGameSpace` / `TranslatePointToScreenSpace`**: replace
   `640`/`480` with `kScreenW`/`kScreenH`, and mirror the letterbox logic
   from step 3. Mouse clicks are wrong until this matches the viewport.
5. **`sbl.h`** `TargetSize{640, 480}` default → `{kScreenW, kScreenH}`.
6. Any other full-screen buffers: `DeltaVid.cpp`, `Intro.cpp`, `CVideo.cpp`,
   `Sbbm.cpp` (`OldFrame`), `StdRaum.cpp` (`OnscreenBitmap`) — decide per
   case whether they are screen-sized (`kScreenW`) or 4:3 content
   (`kLegacyW`, drawn at `kRoomOffsetX`).

---

## 5. Step 3 — Airport view (Phase 1 target)

The airport is a horizontally scrolling world, so it naturally benefits from
the extra width: the player simply sees more of the terminal.

**`AirportView.cpp`**
- `SizeX = 320` (several places, ~L120, 211, 277, 487, 1874): half view width
  used for scroll clamping / centering → `kScreenW / 2`.
- Scroll clamps (~L197–198): `ViewPos.x + 320 > Airport.RightEnd` → uses
  half width; verify whether it should actually be the full view width.
- `RightClip = 640` (~L555) and clip rects `CRect(0, 0, RightClip, 440)`
  (~L737–803, 1224) → `kScreenW`, `kStatusBarY`.
- Tile loops `for (d = 640 / sizes[n] + 2; ...)` (~L748–781): number of
  background tiles to draw → `kScreenW`.
- `RangeDrawn >= ViewPos.x + 640` (~L792) → `kScreenW`.
- Edge scrolling: `gMousePosition.x >= 630` (~L1434) → `kScreenW - 10`;
  check the matching left-edge test.
- Camera snap / jump thresholds `> 640`, `- 320` (~L431–432, 1894) and
  person visibility `< 640` (~L1600) → `kScreenW`, `kScreenW / 2`.
- Ambient sound pan `ViewPos.x + 320` (~L1318) and door sounds
  `ViewPos.x + 320 ... < 400` (~L3755–3889) → center = `kScreenW / 2`;
  scale the 400 range proportionally if desired.
- Zoom origin `MenuSetZoomStuff(XY(320, 240), ...)` (~L83) → screen center.

**Airport extents / world data**
- `Airport.LeftEnd` / `RightEnd` and the clip markers come from the level
  data. A wider view may reveal past the world edges on small airports —
  check that clamping keeps `ViewPos` valid when `RightEnd - LeftEnd`
  is less than `kScreenW`, and that background tiles cover the full width.

**`StdRaum.cpp` — airport-related**
- `RightAirportClip = 640` (~L1987) → `kScreenW`.
- `WindowRect = CRect(0, 0, 640, 480)` (~L368) sets `WinP1/WinP2` for each
  player; the airport view uses the full width → `kScreenW`.

**People / sprites (`Person.cpp`, `Brick.cpp`)**
- Grep for culling against `640`/`320`: anything off-screen-tested must use
  the new width or sprites will pop in at the old edge.

---

## 6. Step 4 — Status bar

Drawn at the bottom of the airport view (`StdRaum.cpp` ~L2740–2800).

- Left (`StatusLineBms[0]`) and right (`StatusLineBms[6]`) pieces are
  already placed relative to `WinP1.x` / `WinP2.x`, so they move to the new
  edges automatically once `WinP2.x == kScreenW`.
- The middle needs to be **filled**: either tile/stretch a middle segment or
  author a wider bar graphic.
- Hardcoded hit tests must be made relative to `WinP2.x`:
  - `gMousePosition.x >= 634 - 24 - 39 ... < 640` (~L2742, 2793)
  - `CursorPos.IfIsWithin(250, 440, 639 - 8 - 24, 479)` etc. (~L2070–2078)
- Inventory items at `WinP1.x + 256 - 8 + c * 60` — fine, left-anchored.
- `UpdateStatusBar()` (~L203) and `StatusBm` (230 px wide) — check text
  layout widths.

---

## 7. Step 5 — Rooms stay 4:3 (interim)

> **Implemented differently:** rooms keep drawing into the left 640 px of the
> 854 px back buffer, untouched. `GameFrame` tracks the visible width
> (`gActiveScreenW`: 854 when the local player's `LocationWin` has
> `IsWideScreen()`, i.e. `AirportView`, else 640) and presents only that part
> of the buffer (`SB_CPrimaryBitmap::SetSource`) into a centered rect. Mouse
> mapping uses the same rect, so no per-room offset is needed. The notes
> below are kept for when a room gets real widescreen art.

Original plan — rooms render as a centered 640×480 block:

- Room backgrounds and `RoomBm` blits get an x-offset of `kRoomOffsetX`
  (or render rooms into a 640-wide bitmap and blit that centered).
- Room mouse handling must subtract `kRoomOffsetX` — simplest approach is a
  single translation in the room's mouse dispatch (`CStdRaum`) rather than
  editing every room.
- Fill the 107 px side bars with black or a decorative border.
- Existing `SetClipRect(CRect(0, 0, 640, 480))` in rooms (`Buero.cpp`,
  `Fracht.cpp`, `Lastmin.cpp`, `Laptop.cpp`, `PlanProp.cpp`, `Reise.cpp`,
  …) become `kLegacyW` relative to the room offset.
- Split / dialog views (`HandyOffset = 320`, `SrcRect(HandyOffset, 0,
  HandyOffset + 340, 440)` ~L1557, 2618) need review — the airport/room
  side-by-side layout depends on the 640 split.

---

## 8. Step 6 — Everything else full-screen

Full-screen 4:3 screens: intro/outro videos, title, credits, menus/options,
pause, statistics, globe/laptop, editor. For each: center at
`kRoomOffsetX` (quick) or relayout (later). `Editor.cpp` has the most
literals (~39) and can stay 4:3 initially.

---

## 9. Testing checklist

- [ ] At `kScreenW = 640` after Step 1: game identical to before.
- [ ] Windowed (resize to 16:9, 16:10, 4:3), borderless and fullscreen.
- [ ] `OptionKeepAspectRatio` on/off.
- [ ] Mouse clicks land correctly in airport view, status bar and rooms.
- [ ] Edge scrolling on both sides; scroll clamping at airport ends.
- [ ] Smallest and largest airports (missions) — no gaps past world edges.
- [ ] People/planes don't pop in at the old 640 edge.
- [ ] Ambient/door sound panning centered.
- [ ] Entering/leaving rooms: correct offset, no garbage in side bars.
- [ ] Split-screen / phone dialog layouts.
- [ ] Multiplayer: `ViewPos` and window data are synced via `AtNet` —
      verify mixed 4:3/16:9 clients don't desync on view-dependent logic.
- [ ] Save/load: `ViewPos` and `WinP1/WinP2` are stored per player — check
      old saves load correctly.

---

## 10. Suggested order

1. Constants + literal replacement (Step 1), test at 640.
2. Back buffer + viewport + mouse (Step 2) with rooms centered (Step 5
   minimal: offset + mouse translation).
3. Airport view (Step 3) and status bar (Step 4).
4. Remaining screens, then room art rework.

---

## 11. Editing airport art (`tools/gli/gli_tool`)

Airport pieces ("bricks") live in `gamefiles/gli/glbrick0..3.gli` (one per
season; season 3 differs, e.g. `WERK1`/`WERK2`). All images are 16-bit
RGB565; colour 0 (pure black) is the transparency key.

Build: `cmake --build build --target gli_tool` (needs the SDL DLLs from
`build/` on `PATH` to run).

```sh
gli_tool list   gamefiles/gli/glbrick0.gli WERK        # names + sizes
gli_tool export gamefiles/gli/glbrick0.gli art-export/glbrick0 [filter]
gli_tool import gamefiles/gli/glbrick0.gli art-export/glbrick0 out.gli
```

- Export writes `<NAME>.png` with real alpha (key colour → transparent).
- Import replaces every image that has a matching `<NAME>.png`. Images may
  change size: the new image is appended and only its directory offset is
  patched, everything else in the file is copied verbatim. Pure black in the
  PNG is nudged to near-black so it doesn't turn transparent; alpha < 128
  becomes transparent.
- Import never overwrites its input; back up the original, then copy
  `out.gli` over it.
- `art-export/` is git-ignored (extracted game art is copyrighted).

Right-end workshop: `WERK1` (wall, 220×274) and `WERK2` (roof, 220×166).
Bricks draw from their left edge, so widening them extends to the right —
about +107 px covers the extra 16:9 view.
