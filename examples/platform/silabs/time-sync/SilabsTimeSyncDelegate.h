/*
 *
 *    Copyright (c) 2026 Project CHIP Authors
 *    All rights reserved.
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

#pragma once

#include <app/clusters/time-synchronization-server/time-synchronization-delegate.h>
#include <lib/core/CHIPError.h>
#include <platform/CHIPDeviceEvent.h>
#include <system/SystemClock.h>
#include <system/SystemLayer.h>

namespace chip {
namespace DeviceLayer {
namespace Silabs {

/**
 * @brief Time Synchronization delegate for Silicon Labs platforms.
 *
 * These boards have no local time source: no GNSS, no PTP, and no NTP client.
 * The only usable source is another node's Time Synchronization cluster, which
 * means the TimeSyncClient (TSC) feature. This delegate therefore:
 *
 *  - Restores an approximate wall clock from Last Known Good UTC Time after a
 *    power loss, without claiming the node is time synchronized.
 *  - Drives the cluster towards the TrustedTimeSource instead of reporting a
 *    platform source, and retries on a backoff until a sync succeeds.
 *  - Refreshes Last Known Good UTC Time periodically while synced, so that the
 *    value restored after the next power loss is reasonably close to reality.
 *
 * Restoring the clock intentionally does not make GetClock_RealTime() succeed:
 * a restored value is an approximation, and reporting it would let the cluster
 * publish a non-null UTCTime with a fabricated TimeSource while Granularity is
 * still NoTimeGranularity.
 */
class SilabsTimeSyncDelegate : public app::Clusters::TimeSynchronization::Delegate
{
public:
    static SilabsTimeSyncDelegate & GetInstance();

    /**
     * Registers this delegate with the Time Synchronization cluster and starts
     * listening for connectivity changes.
     *
     * Safe to call before or after the cluster instance exists.
     */
    CHIP_ERROR Init();

    // TimeSynchronization::Delegate implementation

    /// NTPClient is not supported, so no address can ever be valid.
    bool IsNTPAddressValid(CharSpan ntp) override { return false; }
    bool IsNTPAddressDomain(CharSpan ntp) override { return false; }

    CHIP_ERROR
    UpdateTimeFromPlatformSource(Callback::Callback<app::Clusters::TimeSynchronization::OnTimeSyncCompletion> * callback) override;

    void UTCTimeAvailabilityChanged(uint64_t time) override;
    void TrustedTimeSourceAvailabilityChanged(bool available,
                                              app::Clusters::TimeSynchronization::GranularityEnum granularity) override;
    void NotifyTimeFailure() override;

private:
    SilabsTimeSyncDelegate() = default;

    /// Seeds the wall clock from Last Known Good UTC Time. Best effort.
    void RestoreClockFromLastKnownGoodTime();

    /**
     * Refreshes Last Known Good UTC Time from the current wall clock.
     *
     * This is a non-volatile write, so it runs at most once per refresh
     * interval. It bounds how stale the restored time can be after a power
     * loss, and reuses the record the cluster already maintains rather than
     * adding a platform specific key.
     */
    void RefreshLastKnownGoodTime();

    /// Asks the cluster to run its source prioritization again.
    void RequestTimeSync();

    void ScheduleRetry();
    void CancelRetry();
    void ScheduleLastKnownGoodTimeRefresh();

    static void OnRetryTimer(System::Layer * layer, void * context);
    static void OnLastKnownGoodTimeTimer(System::Layer * layer, void * context);
    static void OnPlatformEvent(const ChipDeviceEvent * event, intptr_t argument);

    bool IsSynced() const;

    /**
     * Completion callback owned by the cluster. Retained so that a retry can
     * re-enter the cluster's source prioritization without the cluster having
     * to expose a trigger for it.
     */
    Callback::Callback<app::Clusters::TimeSynchronization::OnTimeSyncCompletion> * mCompletion = nullptr;

    System::Clock::Seconds32 mRetryInterval{ 0 };
    bool mRetryScheduled        = false;
    bool mRefreshScheduled      = false;
    bool mClockRestoreAttempted = false;
};

} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
