# SonyConnect — Wheel behavior contract (Win32 + Direct2D)

Apply mouse-wheel handling to EVERY numeric slider instance: volume, ANC/ambient intensity, each individual EQ band, and any future slider. **Do not** apply wheel to segmented selectors or on/off switches.

1. Handle `WM_MOUSEWHEEL` (vertical), optionally `WM_MOUSEHWHEEL` (horizontal), and identify the slider under the mouse cursor, not merely keyboard focus. Screen-coordinate conversion (`GET_X_LPARAM`, `GET_Y_LPARAM`) and current hit testing are needed.
2. Only consume a wheel event if the cursor is over an enabled slider; otherwise let parent containers scroll normally. Ignore Ctrl+wheel when assigned to application zoom.
3. Accumulate signed wheel deltas per slider; Windows standard notch is `WHEEL_DELTA` (=120). Most mice: one notch = one logical increment; high-resolution devices provide fractional deltas. Do not discard partial deltas or erroneously treat each message as a full notch.
4. Recommended defaults: volume +1/-1 (0–100); ANC level +1/-1 if the model supports a numeric level; EQ +0.5 dB/-0.5 dB, or the real device-supported increment; avoid pretending that three-state ANC is numeric. Invert signs consistently: wheel up increases, down decreases. Enforce min/max and snap to device-supported quantization.
5. During wheel input, update the locally displayed UI smoothly (tween 120–180ms) while keeping the *requested value* separate from the *confirmed device value*. For drag, visual position should follow pointer directly. Coalesce rapid wheel updates; avoid sending one Bluetooth command for each tiny wheel event. Flush or debounce when appropriate for the real device protocol.
6. A slider must not steal wheel events from another overlapping element; implement clear hover/focus indications. Check disabled/read-only and device-disconnected states.
7. Provide keyboard arrows, PageUp/Down, Home/End, and accessibility labels. No additional installer/runtime dependencies.
8. Each EQ band needs its own wheel hit region and accumulator, so changing a particular band changes only that band. Consider moving the wheel hit area to the entire bar + label, not just a 6-pixel line.

The React examples are visual interaction references. They do not directly compile in the native app. Build and regression-test before applying.
