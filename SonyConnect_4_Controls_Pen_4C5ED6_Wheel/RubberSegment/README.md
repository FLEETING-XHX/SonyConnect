# RubberSegment — Smooth-only React reference

This version removes spring dynamics, overshoot, momentum, dragging, squash and stretch. Selection moves with a **220ms cubic-bezier(0.23, 1, 0.32, 1)** tween by default.

- Drop `RubberSegment.tsx` and `RubberSegment.css` into a React project with `motion` installed (`npm install motion`).
- `Example.tsx` is the updated usage example.
- `duration` is in milliseconds; set `duration={0}` to disable movement.
- Controlled `value` changes are animated. Repeated selections interrupt at the current displayed position.
- Layout resize snaps to the recalculated geometry, avoiding stale sizes.
- Keyboard navigation: arrow keys, Home, End.
- Respects reduced-motion preferences.

**For SonyConnect:** This code is a behavioral reference, not C++ source. Port `clipPath`/edge interpolation to Direct2D; use a time-based tween with the same cubic-bezier easing and redraw during transitions. Keep the existing native UI stack and portable deployment.

## SonyConnect design palette (sampled from supplied screenshot)

| Token | Hex | Usage |
|---|---|---|
| Track | `#EDEFF5` | Segmented control background |
| Selected thumb | `#FFFFFF` | Active pill background |
| Inactive ink | `#626B7E` | Unselected icon and text |
| Active ink | `#4C5ED6` | Selected icon and text |

`Example.tsx` now uses 降噪 / 通透 / 关闭 with inline SVG icons, three equally sized slots, a 58px high large control, and a 220ms non-spring easing transition. Icon outlines are illustrative approximations; replace them with the project's existing icon assets for pixel-level fidelity. The primary #4C5ED6 is the precise Pen design token provided by the user; neutral colors are estimated from the screenshot.
