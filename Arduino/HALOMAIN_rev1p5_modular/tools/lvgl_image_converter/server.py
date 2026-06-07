#!/usr/bin/env python3
"""
Web UI for the LVGL image converter.  Drag-drop an image, tweak options, see a
live preview of exactly what the panel will render, copy/download the .c asset.

    python3 tools/lvgl_image_converter/server.py        # -> http://localhost:9097
    python3 tools/lvgl_image_converter/server.py 9099   # custom port

No external web deps (stdlib http.server). Needs Pillow (convert.py).
"""
import base64
import io
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert as C  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9097


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                self._send(200, f.read(), "text/html; charset=utf-8")
        else:
            self._send(404, "not found", "text/plain")

    def do_POST(self):
        if self.path != "/convert":
            self._send(404, "{}")
            return
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            img_b64 = req["image"].split(",", 1)[-1]  # strip data: prefix
            from PIL import Image
            im = Image.open(io.BytesIO(base64.b64decode(img_b64)))

            opt = req.get("options", {})
            c_src, prev, meta = C.convert_image(
                im,
                name=opt.get("name") or "my_img",
                fmt=opt.get("format", "true_color_alpha"),
                width=opt.get("width") or None,
                height=opt.get("height") or None,
                crop=bool(opt.get("crop")),
                alpha_mode=opt.get("alpha", "source"),
                white_gain=float(opt.get("white_gain", 1.4)),
                recolor=opt.get("recolor") or None,
                swap=opt.get("swap", True),
            )
            buf = io.BytesIO()
            prev.save(buf, "PNG")
            preview_b64 = base64.b64encode(buf.getvalue()).decode()
            self._send(200, json.dumps({
                "ok": True,
                "c_source": c_src,
                "preview": "data:image/png;base64," + preview_b64,
                "meta": meta,
            }))
        except Exception as e:  # noqa: BLE001
            self._send(200, json.dumps({"ok": False, "error": str(e)}))


if __name__ == "__main__":
    print("LVGL image converter UI -> http://localhost:%d  (Ctrl-C to stop)" % PORT)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
