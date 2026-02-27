import os
import re

from SCons.Script import Import

Import("env")


def patch_ws_timeout():
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR")
    pioenv = env.subst("$PIOENV")
    target = os.path.join(
        libdeps_dir,
        pioenv,
        "ArduinoWebsockets",
        "src",
        "tiny_websockets",
        "ws_config_defs.hpp",
    )

    if not os.path.isfile(target):
        print("[ws-timeout-patch] file not found, skip:", target)
        return

    with open(target, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()

    patched = re.sub(
        r"#define\s+_CONNECTION_TIMEOUT\s+\d+",
        "#define _CONNECTION_TIMEOUT 8000",
        content,
        count=1,
    )

    if patched == content:
        if "#define _CONNECTION_TIMEOUT 8000" in content:
            print("[ws-timeout-patch] already applied")
        else:
            print("[ws-timeout-patch] timeout define not found")
        return

    with open(target, "w", encoding="utf-8") as f:
        f.write(patched)

    print("[ws-timeout-patch] applied: _CONNECTION_TIMEOUT=8000")


patch_ws_timeout()

