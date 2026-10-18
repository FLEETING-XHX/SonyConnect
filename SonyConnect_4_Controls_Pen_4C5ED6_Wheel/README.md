# SonyConnect animation controls — Pen #4C5ED6 + Wheel

Components: RubberSegment, WakeSlider, SquishSwitch, ElasticSlider. Main color **#4C5ED6**; background #EDEFF5; idle bars #D9DEEA.

- RubberSegment: smooth segmented selection (no stretch/spring).
- WakeSlider: smooth bar value updates + Wheel support (`wheel`, `wheelStep`), no moving wave.
- SquishSwitch: smooth on/off thumb, no squish or spring.
- ElasticSlider: smooth continuous fill, mouse drag, hover thickness, Wheel (`wheel`, `wheelStep`), no spring/rubber.

Use Wheel for ALL numeric control instances (volume / intensity / EQ). This is a behavior requirement for the native app, not an excuse to change the existing Bluetooth protocol. `WHEEL_DESIGN_CPP.md` specifies Win32 integration.

React examples are source references only. Windows C++20+Win32+Direct2D version requires explicit porting; no React runtime needed in portable build.

**Validation:** archive contents checked; full TypeScript project build and native C++ validation not performed.
