"""
PlatformIO pre-build script: patch arduino-esp32's CMakeLists.txt to skip
compiling the RainMaker and Insights libraries.

Problem:
  Enabling `custom_sdkconfig` in platformio.ini triggers a full framework
  rebuild (needed to reach BT_CTRL_* and NIMBLE pool trims that -D flags
  can't touch). The framework's Arduino build compiles the full library
  set including `RainMaker` and `Insights` — both #include `esp_rmaker_core.h`
  from the `espressif/esp_rainmaker` ESP-IDF component, which we've had
  removed via `custom_component_remove` (v18.9.9.111) because it pulled a
  large dependency chain (esp_diagnostics, esp_schedule, cbor, ...).
  Result: `esp_rmaker_core.h: No such file or directory` -> build fails.

  Upstream CrossPoint's fix (feat-bluetooth 7b6df60a) uses
  `CONFIG_ARDUINO_SELECTIVE_RainMaker=n` in custom_sdkconfig, relying on
  the CMakeLists' guarded loop:
    if(NOT CONFIG_ARDUINO_SELECTIVE_COMPILATION OR CONFIG_ARDUINO_SELECTIVE_${lib})
  That mechanism does NOT work in CrumBLE's pioarduino v55.03.37 arduino-
  esp32 fork: the SELECTIVE_* Kconfig symbols aren't declared, so the
  sdkconfig lines are ignored and the CMake variables stay undefined,
  which means the check evaluates NOT-false OR undefined = true and the
  library compiles anyway.

Fix:
  Directly patch arduino-esp32's CMakeLists.txt to strip `RainMaker` and
  `Insights` from the ARDUINO_ALL_LIBRARIES list. The foreach loop that
  iterates ARDUINO_ALL_LIBRARIES then never sees them, so their sources
  don't get added to ARDUINO_LIBRARIES_SRCS and nothing tries to compile
  RMaker.cpp. Zero runtime impact -- users of RainMaker in an Arduino
  sketch would have failed to link anyway (no `esp_rainmaker` component).

Idempotent -- safe to run on every build; the marker prevents re-patching.
"""

Import("env")
import os


def patch_arduino_esp32(env):
    # Discover framework install path via PlatformIO's package manager.
    fw_pkg = env.PioPlatform().get_package("framework-arduinoespressif32")
    if not fw_pkg:
        print("patch_arduino_esp32: framework not installed yet, skipping")
        return
    cmake_path = os.path.join(fw_pkg.path, "CMakeLists.txt")
    if not os.path.isfile(cmake_path):
        print("patch_arduino_esp32: CMakeLists.txt not found at %s" % cmake_path)
        return
    _strip_libraries_from_all(cmake_path, ["RainMaker", "Insights"])


def _strip_libraries_from_all(cmake_path, libnames):
    marker = "# CrumBLE patch: strip RainMaker+Insights (esp_rainmaker component removed)"
    with open(cmake_path, "r") as f:
        content = f.read()

    if marker in content:
        return

    patched = 0
    for lib in libnames:
        # Match lines like "  Insights\n" or "  RainMaker\n" that live inside
        # the ARDUINO_ALL_LIBRARIES set() block. Comment them out rather than
        # deleting so the diff is greppable if something breaks later.
        old = "  %s\n" % lib
        if old in content:
            content = content.replace(old, "  # %s (removed by CrumBLE patch_arduino_esp32.py)\n" % lib, 1)
            patched += 1

    if patched == 0:
        print(
            "WARNING: patch_arduino_esp32: no target libraries found in %s "
            "-- framework may have been updated" % cmake_path
        )
        return

    # Prepend the marker at the top of the file so subsequent runs no-op.
    content = "# " + marker + "\n" + content
    with open(cmake_path, "w") as f:
        f.write(content)
    print("Patched arduino-esp32: stripped %d libraries (%s) from ARDUINO_ALL_LIBRARIES" %
          (patched, ",".join(libnames)))


patch_arduino_esp32(env)
