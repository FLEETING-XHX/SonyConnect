"""Generate native D2D geometry from the canonical, exported Pen Lucide glyphs.

No network, third-party Python package, external font, or SVG parser is used by
the application. The optional import reads an HTML export made by Pen itself.
"""
from __future__ import annotations

import argparse
import hashlib
from html.parser import HTMLParser
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "native/resources/pen-icons"
OUTPUT = ROOT / "native/src/pen_icons.cpp"
TOKEN = re.compile(r"[MmLlQqZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


class ExportParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.icons = []
        self.current = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "svg" and attrs.get("data-icon-set") == "lucide":
            self.current = {"name": attrs["data-icon-name"],
                            "nodeId": attrs["data-pencil-id"],
                            "viewBox": attrs["viewbox"], "paths": []}
        elif tag == "path" and self.current is not None:
            self.current["paths"].append(attrs["d"])

    def handle_endtag(self, tag):
        if tag == "svg" and self.current is not None:
            self.icons.append(self.current)
            self.current = None


def import_export(path):
    parser = ExportParser()
    parser.feed(path.read_text(encoding="utf-8"))
    ASSETS.mkdir(parents=True, exist_ok=True)
    for icon in parser.icons:
        svg = ET.Element("svg", {"xmlns": "http://www.w3.org/2000/svg",
                                 "viewBox": icon["viewBox"],
                                 "data-pen-node": icon["nodeId"],
                                 "data-icon-set": "lucide",
                                 "data-icon-name": icon["name"]})
        for path in icon["paths"]:
            ET.SubElement(svg, "path", {"d": path, "fill": "currentColor"})
        ET.indent(svg)
        (ASSETS / (icon["name"] + ".svg")).write_text(
            ET.tostring(svg, encoding="unicode") + "\n", encoding="utf-8")


def commands(data):
    tokens = TOKEN.findall(data)
    # Fail closed if a future export adds an unsupported SVG command.
    remainder = TOKEN.sub("", data)
    if re.sub(r"[\s,]", "", remainder):
        raise ValueError(f"Unsupported path data: {remainder}")
    index = 0
    current = (0.0, 0.0)
    start = current
    command = None
    result = []
    while index < len(tokens):
        if tokens[index].isalpha():
            command = tokens[index]
            index += 1
        if command in ("z", "Z"):
            result.append(("close", ()))
            current = start
            command = None
            continue
        if command is None:
            raise ValueError("Missing path command")
        count = 4 if command in ("q", "Q") else 2
        values = tuple(float(v) for v in tokens[index:index + count])
        if len(values) != count:
            raise ValueError("Incomplete path command")
        index += count
        relative = command.islower()
        if command.lower() == "q":
            cx, cy, x, y = values
            if relative:
                cx, cy = cx + current[0], cy + current[1]
                x, y = x + current[0], y + current[1]
            result.append(("quadratic", (cx, cy, x, y)))
            current = (x, y)
        else:
            x, y = values
            if relative:
                x, y = x + current[0], y + current[1]
            op = "move" if command.lower() == "m" else "line"
            result.append((op, (x, y)))
            current = (x, y)
            if op == "move":
                start = current
                command = "l" if relative else "L"
    return result


def number(value):
    text = f"{value:.8f}".rstrip("0").rstrip(".")
    if text in ("", "-0"):
        text = "0"
    if "." not in text:
        text += ".0"
    return text + "f"


def generate():
    entries = []
    arrays = []
    manifest = []
    for path in sorted(ASSETS.glob("*.svg")):
        svg = ET.parse(path).getroot()
        view = [float(v) for v in svg.attrib["viewBox"].split()]
        if view[:2] != [0, 0]:
            raise ValueError("Nonzero icon viewBox origin")
        ops = []
        for child in svg:
            if child.tag.rsplit("}", 1)[-1] != "path":
                raise ValueError("Canonical icon must contain paths only")
            ops.extend(commands(child.attrib["d"]))
        name = path.stem.replace("-", "_")
        lines = []
        for op, values in ops:
            padded = values + (0.0,) * (4 - len(values))
            lines.append("    {Verb::" + op + ", " + ", ".join(number(v) for v in padded) + "},")
        arrays.append("constexpr Command " + name + "_commands[] = {\n" + "\n".join(lines) + "\n};")
        entries.append("    {" + name + "_commands, std::size(" + name + "_commands), \"" +
                       path.stem + "\", " + number(view[2]) + ", " + number(view[3]) + "},")
        manifest.append({"icon": path.stem, "penNode": svg.attrib.get("data-pen-node"),
                         "viewBox": svg.attrib["viewBox"], "commands": len(ops),
                         "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    source = '''// Generated by scripts/Generate-PenIcons.py. Do not hand-edit icon coordinates.
// Geometry: exact Lucide glyph outlines exported by the confirmed Pen prototype.
// See native/resources/pen-icons/README.md and native/notices/Lucide-LICENSE.txt.
#include "sonyconnect/pen_icons.hpp"
#include <d2d1helper.h>
#include <winrt/base.h>
#include <array>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace sonyconnect::ui {
namespace {
enum class Verb { move, line, quadratic, close };
struct Command { Verb verb; float a, b, c, d; };
struct IconData { const Command *commands; std::size_t count; const char *name; float width, height; };
'''
    source += "\n\n".join(arrays)
    source += "\nconstexpr IconData data[] = {\n    {nullptr, 0, \"none\", 14.f, 14.f},\n" + "\n".join(entries) + "\n};\n"
    source += '''static_assert(std::size(data) == static_cast<std::size_t>(PenIcon::count));
} // namespace

struct PenIcons::Impl {
    winrt::com_ptr<ID2D1Factory> factory;
    std::array<winrt::com_ptr<ID2D1PathGeometry>, static_cast<std::size_t>(PenIcon::count)> cache;

    explicit Impl(ID2D1Factory *value) {
        if (!value)
            throw std::invalid_argument("PenIcons requires a D2D factory");
        factory.copy_from(value);
    }

    ID2D1PathGeometry *get(std::size_t index) {
        if (cache[index])
            return cache[index].get();
        winrt::com_ptr<ID2D1PathGeometry> geometry;
        winrt::check_hresult(factory->CreatePathGeometry(geometry.put()));
        winrt::com_ptr<ID2D1GeometrySink> sink;
        winrt::check_hresult(geometry->Open(sink.put()));
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        bool figure_open = false;
        for (std::size_t i = 0; i < data[index].count; ++i) {
            const auto &op = data[index].commands[i];
            switch (op.verb) {
            case Verb::move:
                if (figure_open)
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->BeginFigure(D2D1::Point2F(op.a, op.b), D2D1_FIGURE_BEGIN_FILLED);
                figure_open = true;
                break;
            case Verb::line:
                sink->AddLine(D2D1::Point2F(op.a, op.b));
                break;
            case Verb::quadratic:
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                    D2D1::Point2F(op.a, op.b), D2D1::Point2F(op.c, op.d)));
                break;
            case Verb::close:
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                figure_open = false;
                break;
            }
        }
        if (figure_open)
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        winrt::check_hresult(sink->Close());
        cache[index] = std::move(geometry);
        return cache[index].get();
    }
};

PenIcons::PenIcons(ID2D1Factory *factory) : impl_(std::make_unique<Impl>(factory)) {}
PenIcons::~PenIcons() = default;
PenIcons::PenIcons(PenIcons &&) noexcept = default;
PenIcons &PenIcons::operator=(PenIcons &&) noexcept = default;

void PenIcons::draw(ID2D1RenderTarget *target, ID2D1SolidColorBrush *brush, PenIcon icon,
                    float x, float y, float size, const D2D1_COLOR_F &color) {
    const auto index = static_cast<std::size_t>(icon);
    if (!impl_ || !target || !brush || size <= 0 || index == 0 || index >= std::size(data))
        return;
    auto geometry = impl_->get(index);
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const auto previous_color = brush->GetColor();
    // The SVG export uses preserveAspectRatio=xMidYMid meet.
    const float extent = data[index].width > data[index].height ? data[index].width : data[index].height;
    const float scale = size / extent;
    const float inset_x = (size - data[index].width * scale) / 2;
    const float inset_y = (size - data[index].height * scale) / 2;
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(x + inset_x, y + inset_y) * previous);
    brush->SetColor(color);
    target->FillGeometry(geometry, brush);
    brush->SetColor(previous_color);
    target->SetTransform(previous);
}

const char *pen_icon_name(PenIcon icon) noexcept {
    const auto index = static_cast<std::size_t>(icon);
    return index < std::size(data) ? data[index].name : "none";
}
} // namespace sonyconnect::ui
'''
    OUTPUT.write_text(source, encoding="utf-8")
    (ASSETS / "manifest.json").write_text(json.dumps({
        "penDocument": "63db19b9-f01e-4c69-9d7a-c29249d5a1cd/untitled.pen",
        "exportDate": "2026-10-15", "fillRule": "nonzero", "icons": manifest,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(entries)} canonical icon geometries: {OUTPUT}")


if __name__ == "__main__":
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--import-export", type=Path)
    args = arguments.parse_args()
    if args.import_export:
        import_export(args.import_export)
    generate()
