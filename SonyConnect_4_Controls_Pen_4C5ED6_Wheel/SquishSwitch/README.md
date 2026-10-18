# SonyConnect · Squish Switch — Smooth Edition

Based on the supplied React Bits Squish Switch implementation. This is a **React/TypeScript visual reference**, not C++/Direct2D source code.

## Changes
- Replaced the Motion `spring` animation with `type: 'tween'` and easing `[0.23, 1, 0.32, 1]`.
- Removed `useSpring`, `useVelocity`, `useTransform`, `scaleX`, `scaleY`, motion stretch, and hover scaling.
- Preserved click, pointer drag, Escape cancellation, keyboard activation through native button, `checked` / `defaultChecked`, `onChange`, labels, disabled state, color customization, and reduced-motion support.
- Added `duration` (milliseconds, default **220**); retained independent `colorDuration` (default **180**).
- Set SonyConnect light palette: off track `#D9DEEA`, on track `#4C5ED6`, white thumb `#FFFFFF`.

## Migration notes
- Old options `speed`, `stretch`, and `hoverScale` are intentionally removed. Remove these properties from usage sites.
- `draggable` defaults to `true`; set `false` if only click/tap toggling is desired.
- On pointer drag the thumb follows the pointer immediately. Upon release it smoothly settles to the chosen end (without inertia).
- This component depends on `motion/react` in a React project. No React or Motion runtime is required in a Win32/Direct2D port: translate current X → target X tweening to the existing native rendering loop.
- Colors are SonyConnect palette: primary #4C5ED6 supplied directly from the Pen design; other neutral colors remain estimates from the screenshot.
- Not tested in a complete React build in this environment. Check accessibility and input interactions in the target project.
