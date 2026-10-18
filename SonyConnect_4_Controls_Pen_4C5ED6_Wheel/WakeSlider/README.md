# Wake Slider — SonyConnect Smooth Edition

Based on the WakeSlider source supplied in the conversation. This is a **React/TypeScript reference implementation**, NOT native C++ code.

## What changed

- Removed `useSpring`, `useVelocity`, the speed-dependent crest, and the wave-height calculation.
- All bars stay a fixed height (`restHeight / height`) and only their active color changes.
- Active bar position is advanced by a simple Motion `tween` over `duration=220ms`, using the cubic-bezier `(0.23, 1, 0.32, 1)` ease-out curve.
- Rapid changes retarget the tween from its current visible position; no spring or overshoot.
- Kept min/max/step snapping, optional controlled input, drag/pointer input, keyboard commands, disabled state, showValue and reduced-motion behavior.
- Matching SonyConnect palette (Pen primary color supplied by user): active #4C5ED6, inactive #D9DEEA; sample surface #EDEFF5, text #626B7E.

## Important

This preserves a **discrete 32-bar indicator**. The tween moves the bar activation threshold continuously, but individual bars still switch on/off discretely; a subtle 100ms background-color transition softens the color changes. It is *not* a continuously filled geometric track. If you prefer a continuously interpolated per-bar fill intensity rather than discrete activation, that requires a different paint model.

The original props `sensitivity`, `reach`, `skew`, `glide`, `smoothing`, and `crestColor` are removed because they apply to the removed wave/spring behavior. Remove those when using this new example.

## Native C++ + Win32 + Direct2D transfer

Keep Win32 events and rendering. For each animation, store start %, target %, start timestamp and duration. Calculate normalized progress using a high resolution monotonic clock, invert the cubic-bezier X curve then sample its Y curve, and redraw active bars. Mid-flight retargeting must capture the *current displayed value* before starting a new tween. Respect DPI and snapping; apply the value to the device protocol based on user input, not when animation ends.

## Dependencies and verification

Requires a React project with `motion` installed (`motion/react`). These files have been packaged and statically reviewed, but not built inside a full React project in this environment.
