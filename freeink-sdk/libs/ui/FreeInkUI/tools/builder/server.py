#!/usr/bin/env python3
"""Local FreeInkUI screen builder server."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import mimetypes
import re
import subprocess
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[5]
BUILDER_DIR = Path(__file__).resolve().parent
GALLERY_MANIFEST = ROOT / "docs/images/freeinkui-gallery.json"
IMAGES_DIR = ROOT / "docs/images"
GEN_SCREEN = ROOT / "libs/ui/FreeInkUI/tools/gen_screen.py"
UI_INCLUDE = ROOT / "libs/ui/FreeInkUI/include"
UI_SRC = ROOT / "libs/ui/FreeInkUI/src/FreeInkUI.cpp"
CACHE_DIR = Path(tempfile.gettempdir()) / "freeinkui-builder-render"
def render_inputs():
    """Files whose changes must invalidate cached preview renderers."""
    return [Path(__file__).resolve(), UI_SRC, *sorted(UI_INCLUDE.rglob("*.h"))]


def load_generator():
    spec = importlib.util.spec_from_file_location("freeink_gen_screen", GEN_SCREEN)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {GEN_SCREEN}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GENERATOR = load_generator()


def build_renderer(schema: dict) -> Path:
    generated = GENERATOR.generate(schema)
    safe_area = schema.get("safeArea") or {}
    if not isinstance(safe_area, dict):
        safe_area = {}
    safe_top = int(safe_area.get("top", 0) or 0)
    safe_right = int(safe_area.get("right", 0) or 0)
    safe_bottom = int(safe_area.get("bottom", 0) or 0)
    safe_left = int(safe_area.get("left", 0) or 0)
    match = re.search(r"inline void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", generated)
    if not match:
        raise RuntimeError("generated screen function not found")
    function_name = match.group(1)
    max_match = re.search(r"freeink::ui::Screen<(\d+)>", generated)
    max_interactions = int(max_match.group(1)) if max_match else 64
    cache_key = generated + "".join(f"{path}:{path.stat().st_mtime_ns}" for path in render_inputs())
    digest = hashlib.sha256(cache_key.encode("utf-8")).hexdigest()[:24]
    work = CACHE_DIR / digest
    exe = work / "render_screen"
    if exe.exists():
        return exe

    work.mkdir(parents=True, exist_ok=True)
    header = work / "GeneratedScreen.h"
    runner = work / "render_screen.cpp"
    header.write_text(generated, encoding="utf-8")
    runner.write_text(
        f"""
#include <FreeInkApp.h>
#include <FreeInkUIDisplayTarget.h>
#include "GeneratedScreen.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace freeink::ui;

struct Canvas {{
  int16_t width;
  int16_t height;
  int16_t widthBytes;
  std::vector<uint8_t> fb;
  DisplayTarget target;

  // The framebuffer is already the logical canvas, so force the native
  // (identity) orientation: the default 4-arg constructor auto-rotates
  // landscape panels into Portrait, which broke landscape previews.
  Canvas(int16_t w, int16_t h) : width(w), height(h), widthBytes((w + 7) / 8), fb(widthBytes * h, 0xFF),
                                 target(fb.data(), w, h, widthBytes, Orientation::LandscapeCounterClockwise) {{}}

  bool inkAt(int16_t x, int16_t y) const {{
    const uint8_t byte = fb[static_cast<int32_t>(y) * widthBytes + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    return (byte & mask) == 0;
  }}
}};

int main(int argc, char** argv) {{
  const int16_t width = argc > 1 ? static_cast<int16_t>(std::atoi(argv[1])) : 480;
  const int16_t height = argc > 2 ? static_cast<int16_t>(std::atoi(argv[2])) : 800;
  Canvas canvas(width, height);
  DeviceContext device;
  device.width = width;
  device.height = height;
  device.hasTouch = true;
  device.hasButtons = true;
  device.minTouchSize = 44;
  device.safeArea = Insets{{{safe_top}, {safe_right}, {safe_bottom}, {safe_left}}};
  InputSnapshot input;
  InteractionBuffer<{max_interactions}> interactions;
  Frame<{max_interactions}> frame(canvas.target, device, input, interactions);
  ThemeTokens theme;
  Screen<{max_interactions}> screen(frame, theme);
  {function_name}(screen, nullptr);

  std::cout << "<svg xmlns=\\"http://www.w3.org/2000/svg\\" viewBox=\\"0 0 " << width << " " << height
            << "\\" width=\\"" << width << "\\" height=\\"" << height << "\\">\\n";
  std::cout << "<rect width=\\"100%\\" height=\\"100%\\" fill=\\"#fff\\"/>\\n";
  std::cout << "<g fill=\\"#111\\">\\n";
  for (int16_t y = 0; y < height; ++y) {{
    int16_t x = 0;
    while (x < width) {{
      while (x < width && !canvas.inkAt(x, y)) ++x;
      const int16_t start = x;
      while (x < width && canvas.inkAt(x, y)) ++x;
      if (x > start) {{
        std::cout << "<rect x=\\"" << start << "\\" y=\\"" << y << "\\" width=\\"" << (x - start)
                  << "\\" height=\\"1\\"/>\\n";
      }}
    }}
  }}
  std::cout << "</g>\\n</svg>\\n";
  return 0;
}}
""",
        encoding="utf-8",
    )
    cmd = [
        "c++",
        "-std=c++17",
        "-O2",
        "-I",
        str(UI_INCLUDE),
        str(UI_SRC),
        str(runner),
        "-o",
        str(exe),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)
    return exe


def render_schema(schema: dict, width: int, height: int) -> str:
    exe = build_renderer(schema)
    result = subprocess.run([str(exe), str(width), str(height)], check=True, capture_output=True, text=True)
    return result.stdout


def safe_join(base: Path, rel: str) -> Path | None:
    target = (base / rel.lstrip("/")).resolve()
    try:
        target.relative_to(base.resolve())
    except ValueError:
        return None
    return target


class BuilderHandler(BaseHTTPRequestHandler):
    server_version = "FreeInkUIBuilder/0.1"

    def log_message(self, fmt: str, *args) -> None:
        print(f"{self.address_string()} - {fmt % args}")

    def send_json(self, status: int, payload) -> None:
        body = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, status: int, text: str, content_type: str = "text/plain; charset=utf-8") -> None:
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, path: Path) -> None:
        if not path.is_file():
            self.send_error(404)
            return
        content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_file_headers(self, path: Path) -> None:
        if not path.is_file():
            self.send_error(404)
            return
        content_type = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(path.stat().st_size))
        self.end_headers()

    def static_path(self, path: str) -> Path | None:
        if path == "/":
            return BUILDER_DIR / "index.html"
        if path.startswith("/images/"):
            return safe_join(IMAGES_DIR, path.removeprefix("/images/"))
        return safe_join(BUILDER_DIR, path)

    def do_GET(self) -> None:
        path = unquote(self.path.split("?", 1)[0])
        if path == "/api/gallery":
            self.send_file(GALLERY_MANIFEST)
            return
        target = self.static_path(path)
        self.send_file(target) if target else self.send_error(404)

    def do_HEAD(self) -> None:
        path = unquote(self.path.split("?", 1)[0])
        if path == "/api/gallery":
            self.send_file_headers(GALLERY_MANIFEST)
            return
        target = self.static_path(path)
        self.send_file_headers(target) if target else self.send_error(404)

    def do_POST(self) -> None:
        if self.path not in ("/api/generate", "/api/render"):
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if self.path == "/api/render":
                schema = payload.get("schema", payload)
                width = int(payload.get("width", 480))
                height = int(payload.get("height", 800))
                self.send_text(200, render_schema(schema, width, height), "image/svg+xml; charset=utf-8")
                return
            generated = GENERATOR.generate(payload)
        except Exception as exc:  # noqa: BLE001 - local tool should report schema errors.
            self.send_json(400, {"error": str(exc)})
            return
        self.send_text(200, generated, "text/x-c++hdr; charset=utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8088)
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), BuilderHandler)
    print(f"FreeInkUI builder: http://{args.host}:{args.port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
