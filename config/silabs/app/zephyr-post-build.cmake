#
#   Copyright (c) 2026 Project CHIP Authors
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#

include(${CHIP_ROOT}/config/zephyr/ota-image.cmake)

# ==============================================================================
# Build MCUboot, merge it with the signed application, and create the Matter
# OTA image for Silabs Zephyr targets.
#
# This mirrors config/nxp/app/zephyr-post-build.cmake and assumes a non-sysbuild
# build with the MCUboot sources available at ${ZEPHYR_BASE}/../bootloader/mcuboot.
# ==============================================================================
if(CONFIG_CHIP_OTA_REQUESTOR)
    # When the application is signed by MCUboot (BOOTLOADER_MCUBOOT=y), the build
    # emits zephyr.signed.bin; this is the image wrapped into the OTA file and
    # programmed into the secondary slot by the OTA image processor.
    if(CONFIG_MCUBOOT_SIGNATURE_KEY_FILE STREQUAL "")
        set(ZEPHYR_OUTPUT_NAME "zephyr")
    else()
        set(ZEPHYR_OUTPUT_NAME "zephyr.signed")
    endif()

    set(GLOBAL_BOOTLOADER_CONF_OVERLAY_FILE "${CHIP_ROOT}/config/silabs/app/bootloader.conf")

    # Provide the MCUboot signing key to the bootloader build as an ABSOLUTE path.
    # On Silabs Zephyr, selecting ECDSA P256 makes the bootloader build sign the
    # MCUboot image with Simplicity Commander (zephyr-silabs commander_sign.cmake),
    # which passes CONFIG_BOOT_SIGNATURE_KEY_FILE to `commander convert --keyfile`
    # from the bootloader build directory; MCUboot's imgtool consumes the same
    # option. A relative path resolves against neither tool's working directory, so
    # write the resolved path into a generated overlay merged after bootloader.conf.
    # Replace the development key with a production key for shipping devices.
    get_filename_component(MCUBOOT_SIGNING_KEY
        "${ZEPHYR_BASE}/../bootloader/mcuboot/root-ec-p256.pem" ABSOLUTE)
    set(MCUBOOT_SIGNING_KEY_CONF "${PROJECT_BINARY_DIR}/mcuboot_signing_key.conf")
    file(GENERATE OUTPUT ${MCUBOOT_SIGNING_KEY_CONF}
        CONTENT "CONFIG_BOOT_SIGNATURE_KEY_FILE=\"${MCUBOOT_SIGNING_KEY}\"\n")

    set(ZEPHYR_OUTPUT_DIR ${PROJECT_BINARY_DIR}/zephyr)

    add_custom_target(build_mcuboot ALL
        COMMAND
        west build -b ${BOARD} -d build_mcuboot ${ZEPHYR_BASE}/../bootloader/mcuboot/boot/zephyr
            -- -DOVERLAY_CONFIG="${GLOBAL_BOOTLOADER_CONF_OVERLAY_FILE};${MCUBOOT_SIGNING_KEY_CONF}"
            -DEXTRA_DTC_OVERLAY_FILE="${DTC_OVERLAY_FILE};${EXTRA_DTC_OVERLAY_FILE}"
        COMMAND
        cp ${ZEPHYR_OUTPUT_DIR}/../build_mcuboot/zephyr/zephyr.bin ${ZEPHYR_OUTPUT_DIR}/zephyr.mcuboot.bin
    )
    add_dependencies(build_mcuboot app)

    # Assemble a full-flash image: MCUboot at the boot partition, application at
    # the start of the primary slot (slot0).
    set(BLOCK_SIZE "1024")
    dt_nodelabel(dts_partition_path NODELABEL "boot_partition")
    dt_reg_size(mcuboot_size PATH ${dts_partition_path})
    math(EXPR boot_blocks "${mcuboot_size} / ${BLOCK_SIZE}" OUTPUT_FORMAT DECIMAL)

    add_custom_command(
        OUTPUT ${ZEPHYR_OUTPUT_DIR}/zephyr_full.bin
        DEPENDS build_mcuboot ${ZEPHYR_OUTPUT_DIR}/${ZEPHYR_OUTPUT_NAME}.bin
        COMMAND dd if=${ZEPHYR_OUTPUT_DIR}/zephyr.mcuboot.bin of=${ZEPHYR_OUTPUT_DIR}/zephyr_full.bin
        COMMAND dd if=${ZEPHYR_OUTPUT_DIR}/${ZEPHYR_OUTPUT_NAME}.bin of=${ZEPHYR_OUTPUT_DIR}/zephyr_full.bin bs=${BLOCK_SIZE} seek=${boot_blocks}
    )

    add_custom_target(merge_mcuboot ALL
        DEPENDS ${ZEPHYR_OUTPUT_DIR}/zephyr_full.bin
    )

    if(CONFIG_CHIP_OTA_IMAGE_BUILD)
        chip_ota_image(chip-ota-image
            INPUT_FILES ${ZEPHYR_OUTPUT_DIR}/${ZEPHYR_OUTPUT_NAME}.bin
            OUTPUT_FILE ${ZEPHYR_OUTPUT_DIR}/${CONFIG_CHIP_OTA_IMAGE_FILE_NAME}
        )
    endif()
endif()
