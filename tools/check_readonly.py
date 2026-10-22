# PlatformIO pre-script: bewaakt de schrijfkant van de Marstek-batterij.
#
# De schrijfkant van de Marstek-API (en de aanroepen ervan) mag alleen binnen
# een "#if EMS_BATTERY_CONTROL_LIVE"-blok staan. Een build zonder die vlag (de standaard)
# kan de batterij dus nooit aansturen; battery_control rekent dan alleen in
# schaduw-modus uit wat hij zou sturen. Een build mét de vlag meldt dat luid.
import os

Import("env")  # noqa: F821  (PlatformIO SCons-omgeving)

FORBIDDEN = ("Set" + "Mode", "write_passive" + "_mode", "write_auto" + "_mode", "passive" + "_cfg", "auto" + "_cfg")
GUARD = "#if EMS_BATTERY_CONTROL_LIVE"

project = env.subst("$PROJECT_DIR")  # noqa: F821
src_dir = os.path.join(project, "src", "ems")
hits = []
for root, _dirs, files in os.walk(src_dir):
    for name in files:
        if not name.endswith((".c", ".cpp", ".h", ".hpp")):
            continue
        path = os.path.join(root, name)
        # Eén vlag per open #if: True = de "waar"-tak van #if EMS_BATTERY_CONTROL_LIVE.
        # Een #else/#elif daarvan is juist de schaduw-kant en telt dus niet mee.
        stack = []
        with open(path, encoding="utf-8", errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                stripped = line.strip()
                if stripped.startswith("#if"):
                    stack.append(stripped == GUARD)
                elif stripped.startswith(("#else", "#elif")) and stack:
                    stack[-1] = False
                elif stripped.startswith("#endif") and stack:
                    stack.pop()
                live = any(stack)
                for word in FORBIDDEN:
                    if word in line and not live:
                        hits.append(f"{path}:{lineno}: '{word}'")

if hits:
    print("\n*** Marstek-schrijfcommando buiten een #if EMS_BATTERY_CONTROL_LIVE-blok:")
    for h in hits:
        print("    " + h)
    env.Exit(1)  # noqa: F821

build_flags = env.GetProjectOption("build_flags", "")  # noqa: F821
if not isinstance(build_flags, str):
    build_flags = " ".join(build_flags)
build_flags += " " + os.environ.get("PLATFORMIO_BUILD_FLAGS", "")
if "-DEMS_BATTERY_CONTROL_LIVE=1" in build_flags.replace(" ", ""):
    print("check_readonly: *** LIVE-build - deze firmware STUURT de batterij aan (EMS_BATTERY_CONTROL_LIVE) ***")
else:
    print("check_readonly: OK - schaduw-modus, de batterij wordt niet aangestuurd")
