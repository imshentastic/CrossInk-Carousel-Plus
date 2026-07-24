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
        ws_cpp = os.path.join(
            libdeps_dir, env_dir, "WebSockets", "src", "WebSockets.cpp"
        )
        if os.path.isfile(ws_cpp):
            _apply_static_payload_buffer(ws_cpp)


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


def _apply_static_payload_buffer(filepath):
    """
    Patch WebSockets.cpp to use a single file-scope static buffer for
    incoming frame payloads instead of malloc-per-frame. The library's
    default behaviour is `malloc(payloadLen + 1)` in handleWebsocket()
    and `free(payload)` in handleWebsocketPayloadCb() -- on tight-heap
    targets (ESP32-C3 ~190 KB) sustained BIN uploads disconnect the
    moment heap can't deliver one more contiguous 4 KB chunk. The
    library bails with clientDisconnect(1011) and the upload stalls.

    Fix: keep a static uint8_t buf[WEBSOCKETS_MAX_DATA_SIZE + 1] at
    file scope. When a frame arrives AND the static buf isn't already
    in flight, use it. Otherwise fall back to malloc (multi-client edge
    cases, oversize frames). Frees: only free() if the buffer was
    malloc-allocated, else just release the in-use flag.

    Cost: WEBSOCKETS_MAX_DATA_SIZE + 1 bytes of permanent .bss (~4 KB
    with the project's override). Benefit: eliminates the per-frame
    malloc-failure-disconnect cycle entirely for the common case (one
    client uploading at a time).

    Idempotent.
    """
    marker = "// CrossPoint patch: static payload buffer"
    with open(filepath, "r") as f:
        content = f.read()

    if marker in content:
        return

    decl = (
        "\n" + marker + "\n"
        "static uint8_t g_wsStaticPayloadBuf[WEBSOCKETS_MAX_DATA_SIZE + 1];\n"
        "static bool g_wsStaticPayloadInUse = false;\n"
    )

    # Insert at file scope, OUTSIDE any extern "C" {...} block. WebSockets.cpp
    # wraps the libsha1 include in extern "C" and a naive "after last #include"
    # would drop the static decls inside that block -- `static bool` then trips
    # a C-linkage redefinition error on the C++ compile. Anchor on the comment
    # block that starts the first real function definition instead.
    anchor = "/**\n *\n * @param client WSclient_t *  ptr to the client struct"
    idx = content.find(anchor)
    if idx == -1:
        print("WARNING: WebSockets.cpp static-payload patch: anchor not found in %s" % filepath)
        return
    content = content[:idx] + decl.lstrip("\n") + "\n" + content[idx:]

    malloc_old = (
        "        payload = (uint8_t *)malloc(header->payloadLen + 1);\n"
        "\n"
        "        if(!payload) {\n"
        "            DEBUG_WEBSOCKETS(\"[WS][%d][handleWebsocket] to less memory to handle payload %d!\\n\", "
        "client->num, header->payloadLen);\n"
        "            clientDisconnect(client, 1011);\n"
        "            return;\n"
        "        }"
    )
    malloc_new = (
        "        " + marker + "\n"
        "        if (!g_wsStaticPayloadInUse && header->payloadLen <= WEBSOCKETS_MAX_DATA_SIZE) {\n"
        "            g_wsStaticPayloadInUse = true;\n"
        "            payload = g_wsStaticPayloadBuf;\n"
        "        } else {\n"
        "            payload = (uint8_t *)malloc(header->payloadLen + 1);\n"
        "            if(!payload) {\n"
        "                DEBUG_WEBSOCKETS(\"[WS][%d][handleWebsocket] to less memory to handle payload %d!\\n\", "
        "client->num, header->payloadLen);\n"
        "                clientDisconnect(client, 1011);\n"
        "                return;\n"
        "            }\n"
        "        }"
    )
    if malloc_old not in content:
        print(
            "WARNING: WebSockets.cpp static-payload patch malloc target not found in %s "
            "- library may have been updated" % filepath
        )
        return
    content = content.replace(malloc_old, malloc_new, 1)

    free_old = (
        "        if(payload) {\n"
        "            free(payload);\n"
        "        }"
    )
    free_new = (
        "        if(payload) {\n"
        "            " + marker + " release\n"
        "            if (payload == g_wsStaticPayloadBuf) {\n"
        "                g_wsStaticPayloadInUse = false;\n"
        "            } else {\n"
        "                free(payload);\n"
        "            }\n"
        "        }"
    )
    if free_old not in content:
        print(
            "WARNING: WebSockets.cpp static-payload patch free target not found in %s "
            "- library may have been updated" % filepath
        )
        return
    content = content.replace(free_old, free_new, 1)

    with open(filepath, "w") as f:
        f.write(content)
    print("Patched WebSockets: per-frame malloc replaced with static buffer: %s" % filepath)


patch_websockets(env)
