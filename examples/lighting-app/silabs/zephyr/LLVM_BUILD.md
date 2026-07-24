# Building with LLVM/Clang (Silicon Labs Zephyr SDK)

This document records the changes required to build the Silicon Labs Matter
lighting example with the **LLVM/Clang** toolchain from the
[Silicon Labs Zephyr SDK](https://docs.silabs.com/zephyr/latest/zephyr-getting-started/getting-started-guide)
(`zephyr-sdk-*` with `TOOLCHAIN_VARIANT_COMPILER=llvm`).

The default CI and Docker builds use **GNU GCC**. LLVM is supported locally but
needs the patches below until they are upstreamed.

## Build command

From your `silabs_zephyr` workspace (with `west` and the SDK installed):

```bash
west build -b xg24_rb4187c -p always \
  ~/src/connectedhomeip/examples/lighting-app/silabs/zephyr \
  -- -DCONFIG_CHIP_FACTORY_DATA=y \
     -DTOOLCHAIN_VARIANT_COMPILER=llvm \
     -DUSE_CCACHE=0
```

Use `siwx917_rb4338a` instead of `xg24_rb4187c` for the Wi-Fi target.

Requirements:

- `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` (default with the Silabs SDK)
- `TOOLCHAIN_VARIANT_COMPILER=llvm` (selects Clang from the SDK)
- CMake 4.x and Zephyr 4.4+ as provided by the Silabs Zephyr tree

Verify the toolchain in the build log:

```
-- Found toolchain: zephyr 1.0.1 (...)
-- The C compiler identification is Clang ...
```

and compile commands should include `--target=armv8m.main-none-eabi` (or the
triple for your SoC).

---

## Summary of issues

| Area | Symptom | Root cause |
|------|---------|------------|
| OpenThread CMake | `$<COMPILE_LANGUAGE:CXX>` error in `print-ot-config` | Zephyr libc++ adds CXX-only definitions to `ot-config`; cannot evaluate in `add_custom_target` |
| Mbed TLS / TF-PSA-Crypto | `-Wdocumentation` / `-Wc99-extensions` errors in Silabs PSA headers | Vendor headers + Mbed TLS `-Wdocumentation` + `-Werror` |
| Matter GN (`chip-gn`) | `unsupported argument 'cortex-m33' to option '-mcpu='` | GN invokes Clang without `--target=`; Silabs LLVM needs the triple |
| Matter GN / Zephyr platform | `-Winconsistent-missing-override`, `-Wsign-conversion`, `-Warray-parameter`, `-Wvla-cxx-extension` | Stricter Clang diagnostics vs GCC |
| Application `app` target | Unknown `-Werror=maybe-uninitialized`, `-Wno-stringop-truncation` | GCC-only warning flags in example CMake |

---

## Required changes

Changes span two repositories: **silabs_zephyr** (SDK/Zephyr tree) and
**connectedhomeip** (Matter SDK + example).

### silabs_zephyr

#### 1. OpenThread — guard `print-ot-config`

**File:** `modules/lib/openthread/CMakeLists.txt`

Only create the `print-ot-config` custom target when OpenThread is the top-level
CMake project. When built as a Zephyr module, `ot-config` inherits
`$<COMPILE_LANGUAGE:CXX>` definitions from Zephyr (libc++), which cannot be
evaluated in `add_custom_target` COMMAND.

```cmake
if ("${CMAKE_PROJECT_NAME}" STREQUAL "openthread")
    add_custom_target(print-ot-config ALL
                      COMMAND ${CMAKE_COMMAND}
                      -DLIST="$<TARGET_PROPERTY:ot-config,INTERFACE_COMPILE_DEFINITIONS>"
                      -P ${PROJECT_SOURCE_DIR}/etc/cmake/print.cmake
    )
endif()
```

#### 2. Zephyr Mbed TLS module — vendor header warnings

**File:** `zephyr/modules/mbedtls/CMakeLists.txt`

Before `add_subdirectory(${ZEPHYR_MBEDTLS_MODULE_DIR} mbedtls)`:

```cmake
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Compiler warnings treated as errors" FORCE)
```

After linking mbedtls targets to `zephyr_interface`:

```cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  foreach(lib mbedtls mbedx509 tfpsacrypto builtin p256-m everest pqcp extras platform utilities)
    target_compile_options(${lib} PRIVATE
      -Wno-documentation
      -Wno-c99-extensions
    )
  endforeach()
endif()
```

#### 3. Silabs HAL crypto driver — Clang warning suppressions

**File:** `zephyr-silabs/modules/hal_silabs/simplicity_sdk/CMakeLists.txt`

After `zephyr_library_named(hal_silabs_crypto)`:

```cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  zephyr_library_compile_options(-Wno-documentation -Wno-c99-extensions)
endif()
```

---

### connectedhomeip

#### 4. Matter GN — pass `--target` to Clang

**File:** `config/zephyr/zephyr-util.cmake`

In `zephyr_get_compile_flags()`, after reading compile options:

```cmake
if("${LANG}" STREQUAL "CXX" AND CMAKE_CXX_COMPILER_TARGET)
    list(APPEND FLAGS --target=${CMAKE_CXX_COMPILER_TARGET})
elseif(CMAKE_C_COMPILER_TARGET)
    list(APPEND FLAGS --target=${CMAKE_C_COMPILER_TARGET})
endif()
```

Zephyr sets `CMAKE_C_COMPILER_TARGET` for the Silabs LLVM SDK (e.g.
`armv8m.main-none-eabi`). The Matter GN build calls `clang` directly and does not
get this automatically.

#### 5. Zephyr platform — `override` and sign conversion

| File | Change |
|------|--------|
| `src/platform/Zephyr/BLEManagerImpl.h` | Add `override` to `NotifyChipConnectionClosed()` |
| `src/platform/Zephyr/ZephyrConfig.cpp` | `static_cast<size_t>(bytesRead)`; `BuildCounterConfigKey(..., char key[])` to match header |
| `src/platform/Zephyr/KeyValueStoreManagerImpl.cpp` | `static_cast<size_t>(bytesRead)` in ternary |
| `src/lib/shell/streamer_zephyr.cpp` | `return static_cast<ssize_t>(length);` |
| `src/platform/Zephyr/Logging.cpp` | `#pragma clang diagnostic ignored "-Wvla-cxx-extension"` around Zephyr `LOG_*` calls |

#### 6. Example application — compiler-specific warning flags

**File:** `examples/lighting-app/silabs/zephyr/CMakeLists.txt`

`-Wno-error=maybe-uninitialized` is GCC-only:

```cmake
target_compile_options(app PRIVATE -Werror -Wno-error=format)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(app PRIVATE -Wno-error=maybe-uninitialized)
endif()
```

**File:** `config/silabs/app/enable-gnu-std.cmake`

`-Wno-stringop-truncation` is GCC-only; Clang uses `-Wno-format-truncation`:

```cmake
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gnu17 INTERFACE -Wno-stringop-truncation)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gnu17 INTERFACE -Wno-format-truncation)
endif()
```

#### 7. Silabs example — missing `override`

**File:** `examples/platform/silabs/zephyr/AppTaskZephyr.h`

```cpp
void DispatchEvent(const AppEvent & event) override;
```

---

## Known remaining warnings (non-fatal)

The build completes with warnings from vendor or Zephyr code that was not
modified. These are safe to ignore for local development unless you enable
`-Werror` globally.

| Source | Warning | Notes |
|--------|---------|-------|
| `modules/hal/silabs/.../sli_se_opaque_types.h` | `-Wc99-extensions` (flexible array member) | Included via OpenThread → TF-PSA-Crypto → Silabs PSA driver headers |
| `zephyr/drivers/serial/uart_silabs_usart.c` | `-Wunused-function` | Unused `uart_silabs_ll2cfg_*` helpers |
| `modules/hal/silabs/.../sl_se_manager_key_handling.c` | `-Wparentheses-equality` | Extra parentheses around `==` in vendor SDK |
| `zephyr/modules/openthread/platform/crypto_psa.c` | `-Wunused-result` | Ignored return from `otPlatCryptoDestroyKey()` |

To silence the OpenThread/PSA flexible-array warnings, you could additionally add
`-Wno-c99-extensions` to the OpenThread library targets in
`zephyr/modules/openthread/CMakeLists.txt` (same pattern as the mbedtls change).

---

## Avoiding silabs_zephyr changes #1 and #2 (connectedhomeip-only fixes)

`examples/lighting-app/silabs/zephyr/CMakeLists.txt` implements silabs_zephyr
changes #1 and #2 (and the optional `-Wno-c99-extensions` suggestion below)
entirely from `connectedhomeip`, right after `find_package(Zephyr)`, instead of
patching the vendored OpenThread/Mbed TLS/Zephyr CMake files. If you'd rather
patch those files directly, the snippets below can be removed and replaced
with the corresponding silabs_zephyr changes documented above.

### print-ot-config (OpenThread)

Instead of guarding `print-ot-config` in the vendored
`modules/lib/openthread/CMakeLists.txt`, the same symptom can be worked around
by stripping the CXX-only entries from `ot-config`'s
`INTERFACE_COMPILE_DEFINITIONS`:

```cmake
if(CONFIG_OPENTHREAD AND TARGET ot-config AND TARGET zephyr_interface)
    get_target_property(OT_CONFIG_DEFS ot-config INTERFACE_COMPILE_DEFINITIONS)
    if(OT_CONFIG_DEFS)
        list(FILTER OT_CONFIG_DEFS EXCLUDE REGEX "TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS")
    endif()
    get_target_property(ZEPHYR_INTERFACE_DEFS zephyr_interface INTERFACE_COMPILE_DEFINITIONS)
    if(ZEPHYR_INTERFACE_DEFS)
        list(FILTER ZEPHYR_INTERFACE_DEFS EXCLUDE REGEX "COMPILE_LANGUAGE:CXX")
    endif()
    set_target_properties(ot-config PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "${OT_CONFIG_DEFS};${ZEPHYR_INTERFACE_DEFS}")
endif()
```

This relies on the Generate step running strictly after all of `Configure`
(including the app's own `CMakeLists.txt`), so `ot-config`'s property can still
be rewritten here even though the OpenThread module set it earlier.

`ot-config`'s `INTERFACE_COMPILE_DEFINITIONS` at this point is `PACKAGE_NAME`,
`PACKAGE_VERSION`, `MBEDTLS_SSL_EXPORT_KEYS`, etc. (set by OpenThread's own
CMakeLists.txt) followed by a single list entry that is the literal string
`$<TARGET_PROPERTY:zephyr_interface,INTERFACE_COMPILE_DEFINITIONS>` (set by
`zephyr/modules/openthread/CMakeLists.txt`). **Do not overwrite the whole
property** — an earlier version of this workaround replaced it entirely with
the filtered `zephyr_interface` list, which dropped `PACKAGE_NAME`/
`PACKAGE_VERSION` and broke the OpenThread build with
`error: use of undeclared identifier 'PACKAGE_NAME'` in `instance_api.cpp`.
Only remove that one list entry and re-append a CXX-filtered copy of it.

The only entries dropped from `zephyr_interface`'s list are the LLVM libc++
macros (`_POSIX_C_SOURCE`, `_XOPEN_SOURCE`, `_GNU_SOURCE`) for OpenThread's own
compilation units — OpenThread targets `OT_PLATFORM=zephyr`, not `posix`, so it
should not depend on them, but verify this if OpenThread C++ sources start
failing to compile with missing POSIX declarations.

### Mbed TLS / TF-PSA-Crypto warnings

Instead of setting `MBEDTLS_FATAL_WARNINGS OFF` and patching
`zephyr/modules/mbedtls/CMakeLists.txt`, append the suppressions directly to
the Mbed TLS/TF-PSA-Crypto library targets after `find_package(Zephyr)`
(appending `-Wno-*` after the libraries' own `-Werror`/`-Wdocumentation` flags
still suppresses them, regardless of order):

```cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    foreach(_mbedtls_lib mbedtls mbedx509 tfpsacrypto builtin p256-m everest pqcp extras platform utilities)
        if(TARGET ${_mbedtls_lib})
            target_compile_options(${_mbedtls_lib} PRIVATE -Wno-documentation -Wno-c99-extensions)
        endif()
    endforeach()
endif()
```

### OpenThread `-Wc99-extensions` (non-fatal, but noisy)

OpenThread's core also transitively includes the same Silicon Labs PSA driver
headers (via `crypto/context_size.hpp`), so the same flexible-array-member
warning shows up on its own libraries. This one is non-fatal, but can be
silenced the same way:

```cmake
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CONFIG_OPENTHREAD)
    foreach(_ot_lib openthread-ftd openthread-mtd openthread-radio openthread-cli-ftd openthread-cli-mtd)
        if(TARGET ${_ot_lib})
            target_compile_options(${_ot_lib} PRIVATE -Wno-c99-extensions)
        endif()
    endforeach()
endif()
```

(`openthread-cli-ftd`/`-mtd` include the same crypto headers transitively via
`cli.hpp` → `instance.hpp`, so they need the suppression too.)

---

## Upstreaming

These patches are intended to be contributed upstream:

- **connectedhomeip:** platform Clang fixes (`override`, casts, `zephyr-util.cmake`,
  example CMake) are generally useful for all Zephyr + LLVM builds.
- **silabs_zephyr / zephyr-silabs:** mbedtls and OpenThread CMake guards are
  Silabs/Zephyr integration fixes.
- **Silicon Labs SDK:** PSA driver header documentation (`@param foo[in]`) and
  flexible-array usage are vendor-side cleanups that would reduce need for
  `-Wno-*` suppressions.

---

## Related documentation

- [Main example README](README.md) — default GNU build, flashing, commissioning
- [Silicon Labs Zephyr getting started](https://docs.silabs.com/zephyr/latest/zephyr-getting-started/getting-started-guide)
