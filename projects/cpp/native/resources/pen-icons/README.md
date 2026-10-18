# Pen prototype icon geometry

These 42 SVGs are the actual **Lucide glyph outlines exported by Pen**, from
`63db19b9-f01e-4c69-9d7a-c29249d5a1cd/untitled.pen` on 2026-10-15.
The original `data-icon-name` and `data-pencil-id` are preserved in each file.
Only the color was replaced with `currentColor`; path coordinates are unchanged.

Pen currently renders Lucide through a font whose canonical viewBox is
`0 0 13.99993896484375 14`. Those filled glyph outlines differ slightly from the
current official 24-by-24 stroked SVGs. The application therefore uses **these
exported outlines** for fidelity to the confirmed prototype, rather than tracing
or approximating their shapes. Layout size and color remain the caller's job.

`../../src/pen_icons.cpp` is generated from these files by:

```powershell
python projects/cpp/scripts/Generate-PenIcons.py
```

The optional `--import-export <path.html>` imports a new Pen HTML export before
generation. No network or third-party Python package is required. Do not update
these glyphs independently of an approved prototype update. `manifest.json`
records the exact source-node IDs and SHA256 values.

## Upstream attribution

The paired upstream SVG references are retained in `../lucide/` from the official
Lucide repository, fixed to commit
`d672fc43dbf60962d9d829e35bc3b3e5ed89a178`:

https://github.com/lucide-icons/lucide/tree/d672fc43dbf60962d9d829e35bc3b3e5ed89a178/icons

`history.svg`, removed/renamed in that revision, is retained from the official
`0.468.0` release:

https://github.com/lucide-icons/lucide/blob/0.468.0/icons/history.svg

Lucide uses ISC terms; some icons derive from Feather under MIT terms. The full
unmodified combined upstream license is in `../../notices/Lucide-LICENSE.txt`
and is already included by the existing native package notices installation.
No icon-font binary is bundled. No React, JavaScript, browser, or SVG runtime is
introduced.

## Typography

The prototype uses **Noto Sans SC**, primarily weights 400, 500 and 600, at
12–14 DIPs (the title is 13/500; section headings are 14/600). The development
machine already has the registered `Noto Sans SC` variable font
`C:/Windows/Fonts/NotoSansSC-VF.ttf` (17,773,244 bytes). The renderer exports the
family name as `sonyconnect::ui::pen_font_family`; it does not copy that system
file or install fonts.

Use the installed family and retain a system UI fallback on a machine without
it. Exact typography across arbitrary machines would require an explicit
decision to bundle an officially sourced font with its license; that would add
roughly 17 MB before packaging, and is outside this icon resource change.
