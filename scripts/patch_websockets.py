"""
PlatformIO pre-build script: patch WebSockets for ESP32 Arduino 3.x flush deprecation.

Problem:
  `links2004/WebSockets` 2.7.3 calls `client->tcp->flush()` while disconnecting a
  WebSocket client. In Arduino-ESP32 3.x, `NetworkClient::flush()` is deprecated
  in favor of `clear()`, which produces a warning on every build.

  The upstream discussion notes that `clear()` is ESP32-specific and should not
  replace `flush()` unconditionally for all Arduino targets.

Fix:
  Patch the disconnect path to call:
    - `clear()` on ESP32 Arduino >= 3
    - `flush()` everywhere else

Applied idempotently — safe to run on every build.
"""

Import("env")
import os


def patch_websockets(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return

    for env_dir in os.listdir(libdeps_dir):
        client_cpp = os.path.join(
            libdeps_dir, env_dir, "WebSockets", "src", "WebSocketsClient.cpp"
        )
        if os.path.isfile(client_cpp):
            _apply_flush_guard_fix(client_cpp)
        ws_header = os.path.join(
            libdeps_dir, env_dir, "WebSockets", "src", "WebSockets.h"
        )
        if os.path.isfile(ws_header):
            _apply_max_data_size_guard(ws_header)


def _apply_max_data_size_guard(filepath):
    """
    Add an #ifndef guard around the WebSockets.h WEBSOCKETS_MAX_DATA_SIZE
    #define so a build flag (-DWEBSOCKETS_MAX_DATA_SIZE=N) can override
    the library's hard-coded 15 KB default.

    Lowering this matters on tight-heap targets (ESP32-C3 ~190 KB total)
    where the library's per-frame allocation is what crashes MinFree
    during large WS file uploads -- the original 15 KB peak left only
    ~1.5 KB free at the worst point, which then loses races with
    SD-write/WiFi internal allocations.

    Idempotent.
    """
    marker = "// CrossPoint patch: allow build-flag override of WEBSOCKETS_MAX_DATA_SIZE"
    with open(filepath, "r") as f:
        content = f.read()

    if marker in content:
        return

    old = "#define WEBSOCKETS_MAX_DATA_SIZE (15 * 1024)"
    new = (
        marker + "\n"
        "#ifndef WEBSOCKETS_MAX_DATA_SIZE\n"
        + old + "\n"
        "#endif"
    )

    count = content.count(old)
    if count == 0:
        print(
            "WARNING: WEBSOCKETS_MAX_DATA_SIZE patch target not found in %s "
            "- library may have been updated" % filepath
        )
        return

    content = content.replace(old, new)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched WebSockets: WEBSOCKETS_MAX_DATA_SIZE now overridable (%d sites): %s" % (count, filepath))


def _apply_flush_guard_fix(filepath):
    marker = "// CrossPoint patch: use clear() on ESP32 Arduino >= 3"
    with open(filepath, "r") as f:
        content = f.read()

    if marker in content:
        return

    old = (
        "#if (WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)\n"
        "            client->tcp->flush();\n"
        "#endif"
    )

    new = (
        "#if (WEBSOCKETS_NETWORK_TYPE != NETWORK_ESP8266_ASYNC)\n"
        "            " + marker + "\n"
        "#if defined(ESP32) && defined(ESP_ARDUINO_VERSION_MAJOR) && "
        "(ESP_ARDUINO_VERSION_MAJOR >= 3)\n"
        "            client->tcp->clear();\n"
        "#else\n"
        "            client->tcp->flush();\n"
        "#endif\n"
        "#endif"
    )

    if old not in content:
        print(
            "WARNING: WebSockets flush patch target not found in %s "
            "- library may have been updated" % filepath
        )
        return

    content = content.replace(old, new, 1)
    with open(filepath, "w") as f:
        f.write(content)
    print("Patched WebSockets: ESP32 Arduino 3.x clear()/flush() guard: %s" % filepath)


patch_websockets(env)
