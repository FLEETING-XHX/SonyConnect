# ElasticSlider — smooth SonyConnect reference

This is a deliberately simplified, portable React reference, NOT a byte-for-byte feature clone. The original spring physics, rubber overflow, bump and icon pulsing were removed. New behavior: 220 ms CSS cubic-bezier transition, direct pointer following, interactive track growth on hover, optional vertical layout, Wheel support, ticks and value/label options.

The original `maxStretch`, `bounce` arguments are removed intentionally. Use `wheel`, `wheelStep`, `duration`, `onChange`, `onChangeEnd`.

Value ranges must reflect actual device capabilities. Native Win32 implementation must use `WM_MOUSEWHEEL` and `WHEEL_DELTA` rather than the React normalization logic.
