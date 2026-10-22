"""Zet web/static/ (de website van de Pi, dashboard-web/static) om naar
src/bms/web/web_assets.h, zodat de website in de firmware zit: één image voor
elk bord, niets per apparaat te uploaden.

index.html krijgt app.js/style.css met ?v=<inhoudshash> erachter - hetzelfde
cache-busting-idee als web_server.py op de Pi (die gebruikte de mtime).

    python -I tools/embed_web.py
"""
import hashlib
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATIC = os.path.join(ROOT, "web", "static")
OUT = os.path.join(ROOT, "src", "bms", "web", "web_assets.h")
TYPES = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}


def main():
    files = {}
    for name in sorted(os.listdir(STATIC)):
        with open(os.path.join(STATIC, name), "rb") as f:
            files[name] = f.read()
    version = {n: hashlib.sha256(b).hexdigest()[:10] for n, b in files.items()}
    index = files["index.html"].decode("utf-8")
    index = index.replace('href="style.css"', 'href="style.css?v=%s"' % version["style.css"])
    index = index.replace('src="app.js"', 'src="app.js?v=%s"' % version["app.js"])
    files["index.html"] = index.encode("utf-8")

    lines = [
        "// GEGENEREERD door tools/embed_web.py uit web/static/ - niet met de hand aanpassen.",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    const char * path;",
        "    const char * content_type;",
        "    const uint8_t * data;",
        "    size_t size;",
        "} web_asset_t;",
        "",
    ]
    entries = []
    for i, (name, data) in enumerate(files.items()):
        ctype = TYPES[os.path.splitext(name)[1]]
        lines.append("static const uint8_t WEB_ASSET_%d[] = {" % i)
        for off in range(0, len(data), 24):
            lines.append("    " + ", ".join(str(b) for b in data[off:off + 24]) + ",")
        lines.append("};")
        entries.append('    {"/%s", "%s; charset=utf-8", WEB_ASSET_%d, sizeof(WEB_ASSET_%d)},' % (name, ctype, i, i))
    lines += ["", "static const web_asset_t WEB_ASSETS[] = {"] + entries + ["};", ""]
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("web_assets.h: %s" % ", ".join("%s (%d B)" % (n, len(b)) for n, b in files.items()))


if __name__ == "__main__":
    main()
