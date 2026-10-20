# PlatformIO pre-script: de ESP32 mag de Marstek-batterij nooit aansturen (dat
# doet de Pi). Breekt de build af als een bron onder src/ems/ de schrijf-methode
# van de Marstek Local API noemt, of de schrijfhelpers uit marstek_client.py.
import os

Import("env")  # noqa: F821  (PlatformIO SCons-omgeving)

FORBIDDEN = ("Set" + "Mode", "write_passive" + "_mode", "write_auto" + "_mode", "passive" + "_cfg", "auto" + "_cfg")

src_dir = os.path.join(env.subst("$PROJECT_DIR"), "src", "ems")  # noqa: F821
hits = []
for root, _dirs, files in os.walk(src_dir):
    for name in files:
        if not name.endswith((".c", ".cpp", ".h", ".hpp")):
            continue
        path = os.path.join(root, name)
        with open(path, encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                for word in FORBIDDEN:
                    if word in line:
                        hits.append(f"{path}:{lineno}: '{word}'")

if hits:
    print("\n*** Batterij-alleen-lezen geschonden - de ESP32 mag de batterij niet aansturen:")
    for h in hits:
        print("    " + h)
    env.Exit(1)  # noqa: F821
print("check_readonly: OK - geen Marstek-schrijfcommando's in src/ems")
