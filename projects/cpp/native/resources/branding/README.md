# DSEE Extreme wordmark

`dsee-extreme.png` is the unchanged transparent 2400 x 216 asset from
`docs/ui/DSEE-Extreme-wordmark.png`, matching the Pen DSEE state design.
SHA-256: `040E9FC419F610400AED7197F96CAF560438AF8E9EB6B35773C6F36E3E135508`.

Embedded as Win32 RCDATA 102 and decoded by WIC. It is displayed at 96 x 8.64
DIP inside the 96 x 18 DIP prototype slot. No user-side asset path is required.

For ordinary DSEE, the renderer selects original columns 0..992 (the four DSEE
letters); the Extreme portion starts at x=1112. Both versions retain the same
letter height and sit 8 DIP before the battery group. The PNG is not rewritten.

## Horizontal headphone battery

`battery-penpot.json` preserves the path buffer supplied by the user on
2026-10-21, from Penpot file `ce180602-f36b-8035-8008-cf26122310f3`, shape
`b9310b1e-5d69-80cd-8008-d1ccae93779f`. It contains twenty move/line/cubic
records, an even-odd fill, and a 20 x 12 outline in color `#76767d`.

`scripts/Generate-BatteryIcon.py` decodes that buffer into `battery-penpot.svg`
and `native/include/sonyconnect/battery_icon.hpp`. The header is compiled into
the program; neither Penpot nor an external asset is required at runtime.
Main-window rendering adds the current fill, a low-battery tint, and a charging
mark while retaining the numeric percentage. Unknown/stale levels remain empty
with an unknown-value dash; they are not shown as a measured zero percent.
