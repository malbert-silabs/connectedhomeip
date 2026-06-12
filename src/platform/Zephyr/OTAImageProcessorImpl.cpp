/*
 *
 *    Copyright (c) 2024-2025 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "OTAImageProcessorImpl.h"

#include <app/clusters/ota-requestor/OTADownloader.h>
#include <app/clusters/ota-requestor/OTARequestorInterface.h>
#include <platform/CHIPDeviceLayer.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/version.h>

#ifdef CONFIG_CHIP_OTA_REQUEST_UPGRADE_PERMANENT
#define UPDATE_TYPE BOOT_UPGRADE_PERMANENT
#else
#define UPDATE_TYPE BOOT_UPGRADE_TEST
#endif

// The FIXED_PARTITION_OFFSET()/FIXED_PARTITION_SIZE()/FIXED_PARTITION_DEVICE()
// macros were renamed to PARTITION_OFFSET()/PARTITION_SIZE()/PARTITION_DEVICE()
// and marked deprecated in Zephyr 4.4. Gate on the Zephyr version so the code
// builds warning-free on the latest Zephyr while keeping the original macros for
// older revisions still used by other vendors (e.g. NXP).
//
// OTA_IMAGE_SLOT_DEVICE resolves the flash device that actually owns
// slot1_partition rather than assuming the chosen zephyr,flash-controller. This
// is required when the secondary slot lives on a different device than the
// primary (e.g. an external SPI NOR), and is a no-op when both slots share the
// same controller.
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 4, 0)
#define OTA_IMAGE_SLOT_OFFSET PARTITION_OFFSET(slot1_partition)
#define OTA_IMAGE_SLOT_SIZE PARTITION_SIZE(slot1_partition)
#define OTA_IMAGE_SLOT_DEVICE PARTITION_DEVICE(slot1_partition)
#else
#define OTA_IMAGE_SLOT_OFFSET FIXED_PARTITION_OFFSET(slot1_partition)
#define OTA_IMAGE_SLOT_SIZE FIXED_PARTITION_SIZE(slot1_partition)
#define OTA_IMAGE_SLOT_DEVICE FIXED_PARTITION_DEVICE(slot1_partition)
#endif

static chip::OTAImageProcessorImpl gImageProcessor;

namespace chip {

using namespace ::chip::DeviceLayer;

CHIP_ERROR OTAImageProcessorImpl::Init(OTADownloader * downloader)
{
    VerifyOrReturnError(downloader != nullptr, CHIP_ERROR_INVALID_ARGUMENT);

    gImageProcessor.SetOTADownloader(downloader);

    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::PrepareDownload()
{
    VerifyOrReturnError(mDownloader != nullptr, CHIP_ERROR_INCORRECT_STATE);

    return DeviceLayer::SystemLayer().ScheduleLambda(
        [this] { TEMPORARY_RETURN_IGNORED mDownloader->OnPreparedForDownload(PrepareDownloadImpl()); });
}

CHIP_ERROR OTAImageProcessorImpl::PrepareDownloadImpl()
{
    mHeaderParser.Init();
    mParams = {};

    const struct device * flash_dev;

    // Use the flash device that owns the secondary slot (slot1_partition). This
    // may differ from the chosen zephyr,flash-controller when the secondary slot
    // is on an external flash (e.g. SPI NOR).
    flash_dev = OTA_IMAGE_SLOT_DEVICE;
    if (!device_is_ready(flash_dev))
    {
        ChipLogError(SoftwareUpdate, "OTA secondary slot flash device not ready");
        return System::MapErrorZephyr(-EFAULT);
    }

    int err = stream_flash_init(&mStream, flash_dev, mBuffer, sizeof(mBuffer), OTA_IMAGE_SLOT_OFFSET, OTA_IMAGE_SLOT_SIZE,
                                NULL);

    if (err)
    {
        ChipLogError(SoftwareUpdate, "stream_flash_init failed (err %d)", err);
    }

    return System::MapErrorZephyr(err);
}

CHIP_ERROR OTAImageProcessorImpl::Finalize()
{
    int err = stream_flash_buffered_write(&mStream, NULL, 0, true);

    if (err)
    {
        ChipLogError(SoftwareUpdate, "stream_flash_buffered_write failed (err %d)", err);
    }

    return System::MapErrorZephyr(err);
}

CHIP_ERROR OTAImageProcessorImpl::Abort()
{
    ChipLogError(SoftwareUpdate, "Image upgrade aborted");

    return CHIP_NO_ERROR;
}

CHIP_ERROR OTAImageProcessorImpl::Apply()
{
    // Schedule update of image
    int err = boot_request_upgrade(UPDATE_TYPE);

#ifdef CONFIG_CHIP_OTA_REQUESTOR_REBOOT_ON_APPLY
    if (!err)
    {
        PlatformMgr().HandleServerShuttingDown();
        /*
         * Restart the device in order to apply the update image.
         * This should be done with a delay so the device has enough time to send
         * the state-transition event when applying the update.
         */
        ChipLogProgress(SoftwareUpdate, "Restarting device, will reboot in %d seconds ...", mDelayBeforeRebootSec);
        return SystemLayer().StartTimer(
            System::Clock::Milliseconds32(mDelayBeforeRebootSec * 1000 + kDeltaRebootDelayMs),
            [](System::Layer *, void * /* context */) {
                k_msleep(CHIP_DEVICE_CONFIG_SERVER_SHUTDOWN_ACTIONS_SLEEP_MS);
                sys_reboot(SYS_REBOOT_WARM);
            },
            nullptr /* context */);
    }
    else
    {
        return System::MapErrorZephyr(err);
    }
#else
    return System::MapErrorZephyr(err);
#endif
}

CHIP_ERROR OTAImageProcessorImpl::ProcessBlock(ByteSpan & aBlock)
{
    VerifyOrReturnError(mDownloader != nullptr, CHIP_ERROR_INCORRECT_STATE);

    CHIP_ERROR error = ProcessHeader(aBlock);

    if (error == CHIP_NO_ERROR)
    {
        error = System::MapErrorZephyr(stream_flash_buffered_write(&mStream, aBlock.data(), aBlock.size(), false));
        mParams.downloadedBytes += aBlock.size();
    }

    // Report the result back to the downloader asynchronously.
    return DeviceLayer::SystemLayer().ScheduleLambda([this, error, aBlock] {
        if (error == CHIP_NO_ERROR)
        {
            ChipLogDetail(SoftwareUpdate, "Downloaded %u/%u bytes", static_cast<unsigned>(mParams.downloadedBytes),
                          static_cast<unsigned>(mParams.totalFileBytes));
            TEMPORARY_RETURN_IGNORED mDownloader->FetchNextData();
        }
        else
        {
            mDownloader->EndDownload(error);
        }
    });
}

bool OTAImageProcessorImpl::IsFirstImageRun()
{
    OTARequestorInterface * requestor = GetRequestorInstance();
    VerifyOrReturnError(requestor != nullptr, false);

    uint32_t currentVersion;
    VerifyOrReturnError(ConfigurationMgr().GetSoftwareVersion(currentVersion) == CHIP_NO_ERROR, false);

    return requestor->GetCurrentUpdateState() == OTARequestorInterface::OTAUpdateStateEnum::kApplying &&
        requestor->GetTargetVersion() == currentVersion;
}

CHIP_ERROR OTAImageProcessorImpl::ConfirmCurrentImage()
{
    return System::MapErrorZephyr(boot_write_img_confirmed());
}

CHIP_ERROR OTAImageProcessorImpl::ProcessHeader(ByteSpan & aBlock)
{
    if (mHeaderParser.IsInitialized())
    {
        OTAImageHeader header;
        CHIP_ERROR error = mHeaderParser.AccumulateAndDecode(aBlock, header);

        // Needs more data to decode the header
        VerifyOrReturnError(error != CHIP_ERROR_BUFFER_TOO_SMALL, CHIP_NO_ERROR);
        ReturnErrorOnFailure(error);

        mParams.totalFileBytes = header.mPayloadSize;
        mHeaderParser.Clear();
    }

    return CHIP_NO_ERROR;
}

void OTAImageProcessorImpl::SetRebootDelaySec(uint16_t rebootDelay)
{
    mDelayBeforeRebootSec = rebootDelay;
}

OTAImageProcessorImpl & OTAImageProcessorImpl::GetDefaultInstance()
{
    return gImageProcessor;
}

} // namespace chip
