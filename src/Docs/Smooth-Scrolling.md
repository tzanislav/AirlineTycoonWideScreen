# Smooth Scrolling — Findings

Why the airport view feels choppy even when the frame counter says 144 FPS,
and what it would take to fix it. No code has been changed yet.

---

## 1. Summary

The airport scene is only **re-rendered 20 times per second**. The other
frames re-present the same image. So the 144 FPS is real, but only about 1 in
7 frames shows anything new.

On top of that, every position (camera, people, planes) is a whole number of
game pixels that changes once per 50 ms step. With the window scaled up
(480 → 1080 lines is ×2.25), each step moves the picture several screen
pixels at once.

The fix is to **render every display frame and interpolate positions between
simulation steps**, while keeping the simulation itself at 20 Hz. That's
standard and doable, but it needs care, because several pieces of drawing code
assume exactly one repaint per simulation step (§5).

---

## 2. How the game loop works

`CTakeOffApp::GameLoop` ([Takeoff.cpp](../Takeoff.cpp)):

| Step | Where | What |
|---|---|---|
| Fixed timestep | `Takeoff.cpp` ~L883–887 | `NumSimSteps += (Time - LastTime) / 50` — *"20 Schritte pro Sekunde"* |
| Simulation | `Takeoff.cpp` ~L1345+ | Runs `NumSimSteps` steps: `Sim.Persons.DoOneStep()` (~L1808), planes, clock, AI. Sets `RefreshNeccessary = TRUE` |
| Camera | `Takeoff.cpp` ~L2059–2066 | **Only if `RefreshNeccessary`**: `AirportView::MoveCamera()` per airport player |
| Render | same block | `FrameWnd->Invalidate()` → `GameFrame::OnPaint` → `AirportView::OnPaint` redraws everything, then `PrimaryBm.Flip` |
| Idle frames | `Takeoff.cpp` ~L2071–2073 | Otherwise `MessagePump(); PrimaryBm.PrimaryBm.Present();` — **re-presents the old frame** (only the hardware mouse cursor moves) |

That last row is why the counter reads 144: `Present()` runs on every loop
pass at vsync rate, but the scene underneath only changes at 20 Hz. The mouse
cursor is drawn during present, which is why the cursor feels smooth and the
world doesn't.

Outside the game (menus, before boot) `RefreshNeccessary` is always true
(~L2053), so those screens already repaint every frame.

---

## 3. What makes the motion look stepped

All of these update once per 50 ms step, in whole game pixels:

| Source | Where | Per-step behaviour |
|---|---|---|
| Camera position | `PLAYER::ViewPos` (`XY`, integers) | Moved in `AirportView::FocusCameraOnPos` |
| Camera follow speed | `CameraSpeed` | Integer px/step, changes by ±1 per step (inertia via `CalcInertiaVelocity`, ~L264, ~L324) |
| Mouse/edge scrolling | `gMouseScrollSpeed` | Ramps 2 → **44 px/step** (~L160–180), i.e. up to 880 px/s in 44 px jumps (~100 screen px at ×2.25) |
| Snap scrolling (`OptionFlipping`) | ~L222–228 | Jumps a whole screen at once (intentional) |
| People | `PERSON::ScreenPos` from `Position` (`Person.cpp` ~L786, ~L928) | Step size from `WalkSpeed` per clan (`StepSize`, ~L942; player ×2 horizontally, ~L1793) |
| Sprite animation | `Phase` per person | Advances per step — intentionally low-frame-rate, that's fine |
| Brick animations | `Sim.TickerTime = Time >> 5` (~L2054) | Already time-based (~31 Hz), not tied to steps |

So even if we rendered at 144 Hz today, nothing would move between steps:
the positions don't change.

---

## 4. Constraints

**Multiplayer lockstep.** Network games keep players in sync by simulation
step (`NumSimSteps` capped by `WaitWorkTill`, `gTimerCorrection`; ~L1270–1340).
The simulation must stay at a fixed 20 Hz with identical results. Any
smoothing has to be **render-only**: interpolated values must never feed back
into simulation state.

**Camera state is saved.** `ViewPos` and `CameraSpeed` are written to save
games ([Player.cpp:7672](../Player.cpp#L7672), [7882](../Player.cpp#L7882)). Keep the
real integer `ViewPos` as the saved and authoritative value, and add separate
render-only fields.

**`ViewPos` is read by gameplay-adjacent code**, for example ambient and door
sound distances in `AirportView.cpp` and a check against the camera in
[Sim.cpp:2967](../Sim.cpp#L2967). Those should keep using the real `ViewPos`.

**Software rendering.** All drawing is CPU blitting into a 16-bit
854×480 surface, then one texture upload. A full airport repaint at 144 Hz is
probably fine on modern hardware but should be measured: the `Bench` / `BrickWait`
timers already exist in `AirportView::OnPaint`.

---

## 5. Drawing code that assumes one paint per step

Painting more often would speed these up or change them. Each needs to move
into the simulation step, or be driven by elapsed time:

| Code | Where | Issue |
|---|---|---|
| **Spawning shoppers** | `AirportView::OnPaint` ~L684: `if (PersonsToAdd && rand() % 4 == 0 ...) Sim.Persons *= PERSON(...)` | **Changes the simulation from paint.** Must move into the step. |
| `FlackerCount++` | `AirportView::OnPaint` ~L682 | Flicker animation would run ~7× faster |
| Smoke/stench particles | `AirportView::OnPaint` ~L979, ~L1005 (`rand() % 4` spawn, TTL) | Particle spawning and lifetime would run ~7× faster |
| Status bar timeout | `StdRaum.cpp` ~L2768 `StatusCount--` | Status bar would auto-hide ~7× faster |
| Fades | `SBPRIMARYBM::Flip` ([Sbbm.cpp](../Sbbm.cpp) ~L486+) `gBlendState--` per flip | Room/camera fades would be ~7× shorter |
| Pause fade | `GameFrame::OnPaint` `PauseFade++` | Same |
| Tooltip / menu timers, think-bubble countdowns | various `OnPaint` | Audit: anything that counts paints rather than time |

Searching `OnPaint` bodies for `++`, `--`, `rand()` and `*=` finds most of them.

---

## 6. Options

### A. Render every frame + interpolate (recommended)

Keep the 20 Hz simulation. Each display frame, compute
`alpha = (Time - LastTime) / 50.0` (0..1, the progress toward the next step)
and draw at positions blended between the previous and current step.

1. **Store previous positions** at the start of each sim step:
   `PrevViewPos` per player; `PrevScreenPos` per `PERSON` (and plane positions on the runway).
2. **Render-time positions**: `lerp(Prev, Current, alpha)`, rounded to whole
   pixels for the software blitter. At 144 Hz this gives ~7 intermediate
   positions per step instead of one jump.
3. **Repaint every loop pass** in the airport, not only when
   `RefreshNeccessary` is set (`Takeoff.cpp` ~L2059). Rooms can stay as they are.
4. **Teleports**: when a person warps (enters or leaves a room, spawns) or the
   camera jumps (fade, `CenterCameraOnPlayer`, snap scrolling), set
   `Prev = Current` so nothing slides across the screen.
5. **Fix the §5 list first**, or the game will visibly speed up.
6. **Hit-testing** (clicks on people and bricks) can stay on the real
   positions; the difference is under one step's movement.

Expected result: camera and people glide; sprite animation frames stay at
their original rate (that's the intended art style).

### B. Interpolate the camera only

Cheaper, but the player character still steps at 20 Hz while the camera moves
smoothly, so the character would visibly judder against the background.
Not recommended on its own; it can be a first milestone on the way to A.

### C. Raise the simulation rate

Changes walking speeds, timings, AI pacing and network sync. Every per-step
constant in the game would need rescaling. Not recommended.

### D. Sub-pixel presentation (follow-up to A)

After A, motion still moves in whole *game* pixels (2.25 screen px at 1080p).
For true sub-pixel smoothness:

- Render the airport into a buffer a few pixels wider than the view, at the
  rounded camera position, then present it with the fractional remainder as an
  offset using `SDL_RenderCopyF` in `SB_CPrimaryBitmap::Present`
  ([CBitmap.cpp](../SBLib/source/CBitmap.cpp)).
- The status bar is drawn into the same buffer, so it would shift too. It
  needs to become a separate layer or texture first.
- People are positioned by whole pixels relative to the camera, so their
  sub-pixel offset differs per sprite. Only the camera can be shifted this way.

Worth trying only if A still looks stepped at large window sizes.

---

## 7. Related: scaling quality

`SDL_HINT_RENDER_SCALE_QUALITY` is set to `"2"` (linear) in
`GameFrame::GameFrame`. At non-integer scales (×2.25) this blurs slightly and
can make scrolling edges shimmer. An optional integer-scale mode (×2 with
borders), or nearest-neighbour with integer scaling, would give crisper
motion. That's independent of the above.

---

## 8. Suggested order

1. Move the §5 paint-side effects into the sim step / make them time-based
   (no visible change at 20 Hz paint).
2. Camera interpolation (`PrevViewPos`, render-time `ViewPos`) plus
   repaint-every-frame in the airport.
3. Person interpolation (`PrevScreenPos`) and teleport handling.
4. Measure render cost at 144 Hz; optimise if needed.
5. Optional: sub-pixel camera (§6 D), integer-scale option (§7).

## 9. Test checklist

- [ ] Walking left/right: camera and player move smoothly, no judder between them.
- [ ] Edge scrolling and click-drag scrolling at full speed.
- [ ] Entering/leaving rooms, long-distance fade, `CenterCameraOnPlayer`: no sliding.
- [ ] Status bar auto-hide, fades, smoke, flicker run at the original speed.
- [ ] Shopper spawn rate unchanged (compare people count after N game minutes).
- [ ] Multiplayer: two clients stay in sync; savegames load at the same camera position.
- [ ] Frame-time with 4 players' worth of people in a busy airport.
