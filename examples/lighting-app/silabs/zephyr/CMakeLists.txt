#
# Copyright (c) 2025 Project CHIP Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
cmake_minimum_required(VERSION 3.20)

get_filename_component(CHIP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/connectedhomeip REALPATH)
get_filename_component(GEN_DIR ${CHIP_ROOT}/zzz_generated/ REALPATH)
get_filename_component(ZEPHYR_PLATFORM_COMMON ${CHIP_ROOT}/examples/platform/silabs/zephyr REALPATH)
get_filename_component(SILABS_LIGHTING_COMMON ${CHIP_ROOT}/examples/lighting-app/silabs REALPATH)
get_filename_component(ALL_CLUSTERS_COMMON_DIR ${CHIP_ROOT}/examples/all-clusters-app/all-clusters-common REALPATH)


# Need this as wiseconnect abstraction is causing a build failure
add_definitions(-DMBEDTLS_X509_REMOVE_INFO )

list(APPEND ZEPHYR_EXTRA_MODULES ${CHIP_ROOT}/config/silabs)
find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

# With TOOLCHAIN_VARIANT_COMPILER=llvm, Zephyr's C++ support (CONFIG_LIBCXX_LIBCPP)
# adds $<$<COMPILE_LANGUAGE:CXX>:...> definitions to zephyr_interface. The OpenThread
# module copies zephyr_interface's INTERFACE_COMPILE_DEFINITIONS verbatim into
# ot-config (as a single $<TARGET_PROPERTY:zephyr_interface,...> list entry), and
# OpenThread's own CMakeLists.txt then queries ot-config's INTERFACE_COMPILE_DEFINITIONS
# from inside an add_custom_target() COMMAND (print-ot-config), which is not a valid
# context for $<COMPILE_LANGUAGE:...>. Replace that one list entry with a flattened,
# CXX-filtered copy so the generator expression is never evaluated there, instead of
# patching the vendored OpenThread/Zephyr CMake files. ot-config's own definitions
# (PACKAGE_NAME, etc., set earlier in the same property by OpenThread's CMakeLists.txt)
# are preserved since only the zephyr_interface indirection entry is removed.
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

# Mbed TLS / TF-PSA-Crypto build with MBEDTLS_FATAL_WARNINGS (-Werror), and the
# Silicon Labs PSA driver vendor headers (sl_psa_driver) have malformed Doxygen
# tags (e.g. "@param key_desc[in]" instead of "@param[in] key_desc"), which
# Clang's -Wdocumentation rejects. Also silence -Wc99-extensions for the same
# headers' flexible array members. Appending -Wno-* after the libraries' own
# -Werror/-Wdocumentation flags suppresses just these diagnostics.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    foreach(_mbedtls_lib mbedtls mbedx509 tfpsacrypto builtin p256-m everest pqcp extras platform utilities)
        if(TARGET ${_mbedtls_lib})
            target_compile_options(${_mbedtls_lib} PRIVATE -Wno-documentation -Wno-c99-extensions)
        endif()
    endforeach()
endif()

# OpenThread's core (crypto/context_size.hpp) transitively includes the same
# Silicon Labs PSA driver headers, triggering the same -Wc99-extensions
# flexible-array-member warning (non-fatal, but noisy) on its own libraries.
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CONFIG_OPENTHREAD)
    foreach(_ot_lib openthread-ftd openthread-mtd openthread-radio openthread-cli-ftd openthread-cli-mtd)
        if(TARGET ${_ot_lib})
            target_compile_options(${_ot_lib} PRIVATE -Wno-c99-extensions)
        endif()
    endforeach()
endif()

# -Wmaybe-uninitialized has too many false positives, including on std::optional
# and chip::Optional.  Make it nonfatal.
#
# See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80635
target_compile_options(app PRIVATE -Werror -Wno-error=format)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(app PRIVATE -Wno-error=maybe-uninitialized)
endif()

project(matter-zephyr-light)

include(${CHIP_ROOT}/config/silabs/app/enable-gnu-std.cmake)
include(${CHIP_ROOT}/src/app/chip_data_model.cmake)

target_include_directories(app
    PRIVATE
    include
    ${ZEPHYR_PLATFORM_COMMON}
)

target_sources(app
    PRIVATE
    src/AppTask.cpp
    src/DeviceCallbacks.cpp
    ../include/CHIPProjectConfig.h
    ${ZEPHYR_PLATFORM_COMMON}/main.cpp
    ${ZEPHYR_PLATFORM_COMMON}/AppTaskBase.cpp
    ${ZEPHYR_PLATFORM_COMMON}/AppTaskZephyr.cpp
    ${ZEPHYR_PLATFORM_COMMON}/CHIPDeviceManager.cpp
    ${ZEPHYR_PLATFORM_COMMON}/CommonDeviceCallbacks.cpp
    ${ZEPHYR_PLATFORM_COMMON}/DataModelCallbacks.cpp
)

if(CONFIG_CHIP_OTA_REQUESTOR)
    target_sources(app PRIVATE
                    ${ZEPHYR_PLATFORM_COMMON}/ota_requestor/source/OTARequestorInitiatorCommon.cpp
                    ${ZEPHYR_PLATFORM_COMMON}/ota_requestor/source/OTARequestorInitiatorZephyr.cpp
    )
    target_include_directories(app PRIVATE
                    ${ZEPHYR_PLATFORM_COMMON}/ota_requestor/include/
    )
endif()

if(CONFIG_CHIP_WIFI)
  chip_configure_data_model(app INCLUDE_SERVER
      ZAP_FILE ${SILABS_LIGHTING_COMMON}/data_model/lighting-wifi-app.zap
  )
else()
  chip_configure_data_model(app INCLUDE_SERVER
      ZAP_FILE ${SILABS_LIGHTING_COMMON}/data_model/lighting-thread-app.zap
  )
endif()

if(CONFIG_CHIP_LIB_SHELL)
    target_compile_definitions(app PRIVATE ENABLE_CHIP_SHELL)
    target_include_directories(app PRIVATE 
                               ${CHIP_ROOT}/examples/shell/shell_common/include
                               ${ZEPHYR_PLATFORM_COMMON}/include
    )
    target_sources(app PRIVATE
                   ${ZEPHYR_PLATFORM_COMMON}/AppCLIZephyr.cpp
                   ${CHIP_ROOT}/examples/shell/shell_common/cmd_misc.cpp
                   ${CHIP_ROOT}/examples/shell/shell_common/cmd_otcli.cpp
                   ${CHIP_ROOT}/examples/shell/shell_common/cmd_server.cpp
    )
endif()

include(${CHIP_ROOT}/config/silabs/app/zephyr-post-build.cmake)
