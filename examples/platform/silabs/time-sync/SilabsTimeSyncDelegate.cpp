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

#include "SilabsTimeSyncDelegate.h"

// Lives in the cluster's app_config_dependent_sources, so it is compiled into
// the application data model target rather than the cluster source set.
#include <app/clusters/time-synchronization-server/CodegenIntegration.h> // nogncheck
#include <app/server/Server.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/TimeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/silabs/SystemTimeSupport.h>

#include <algorithm>

/// First retry delay after a failed time synchronization attempt.
#ifndef SL_MATTER_TIME_SYNC_RETRY_INITIAL_SECONDS
#define SL_MATTER_TIME_SYNC_RETRY_INITIAL_SECONDS 30
#endif

/// Retry ceiling. The spec states a node SHOULD NOT query the Time
/// Synchronization cluster of another node more than once per 30 minutes.
#ifndef SL_MATTER_TIME_SYNC_RETRY_MAX_SECONDS
#define SL_MATTER_TIME_SYNC_RETRY_MAX_SECONDS 1800
#endif

/// How often Last Known Good UTC Time is refreshed while the clock is synced.
/// Each refresh is a non-volatile write, matching the cadence already used for
/// TotalOperationalHours.
#ifndef SL_MATTER_TIME_SYNC_LKGT_REFRESH_SECONDS
#define SL_MATTER_TIME_SYNC_LKGT_REFRESH_SECONDS 3600
#endif

using chip::System::Clock::RealTimeSource;

namespace TimeSync = chip::app::Clusters::TimeSynchronization;

namespace chip {
namespace DeviceLayer {
namespace Silabs {

namespace {

constexpr System::Clock::Seconds32 kRetryInitial{ SL_MATTER_TIME_SYNC_RETRY_INITIAL_SECONDS };
constexpr System::Clock::Seconds32 kRetryMax{ SL_MATTER_TIME_SYNC_RETRY_MAX_SECONDS };
constexpr System::Clock::Seconds32 kLastKnownGoodTimeRefresh{ SL_MATTER_TIME_SYNC_LKGT_REFRESH_SECONDS };

} // namespace

SilabsTimeSyncDelegate & SilabsTimeSyncDelegate::GetInstance()
{
    static SilabsTimeSyncDelegate instance;
    return instance;
}

CHIP_ERROR SilabsTimeSyncDelegate::Init()
{
    TimeSync::SetDefaultDelegate(this);

    // The cluster only runs its source prioritization once, on kServerReady,
    // which on Thread can fire before the node has attached. Watching
    // connectivity lets us retry as soon as the network is actually usable.
    return PlatformMgr().AddEventHandler(OnPlatformEvent, reinterpret_cast<intptr_t>(this));
}

bool SilabsTimeSyncDelegate::IsSynced() const
{
    return System::Clock::GetRealTimeSource() == RealTimeSource::kSynced;
}

CHIP_ERROR SilabsTimeSyncDelegate::UpdateTimeFromPlatformSource(Callback::Callback<TimeSync::OnTimeSyncCompletion> * callback)
{
    // Retained so retries can re-enter the cluster's source prioritization.
    mCompletion = callback;

    if (!mClockRestoreAttempted)
    {
        mClockRestoreAttempted = true;
        RestoreClockFromLastKnownGoodTime();
    }

    // No local time source exists on these boards: no GNSS, no PTP, no NTP
    // client. Returning an error makes the cluster move straight on to the
    // TrustedTimeSource, which is the only source we can actually use.
    return CHIP_ERROR_NOT_IMPLEMENTED;
}

void SilabsTimeSyncDelegate::UTCTimeAvailabilityChanged(uint64_t time)
{
    // The cluster has already written the system clock and Last Known Good UTC
    // Time by this point. Stop retrying and start bounding the drift of the
    // value we would restore after a power loss.
    CancelRetry();
    mRetryInterval = System::Clock::Seconds32(0);
    ScheduleLastKnownGoodTimeRefresh();
}

void SilabsTimeSyncDelegate::TrustedTimeSourceAvailabilityChanged(bool available, TimeSync::GranularityEnum granularity)
{
    VerifyOrReturn(available && !IsSynced());
    RequestTimeSync();
}

void SilabsTimeSyncDelegate::NotifyTimeFailure()
{
    ChipLogProgress(DeviceLayer, "Time synchronization failed; will retry");
    ScheduleRetry();
}

void SilabsTimeSyncDelegate::RestoreClockFromLastKnownGoodTime()
{
    System::Clock::Seconds32 lastKnownGoodChipEpoch;
    CHIP_ERROR err = Server::GetInstance().GetFabricTable().GetLastKnownGoodChipEpochTime(lastKnownGoodChipEpoch);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(DeviceLayer, "No Last Known Good UTC Time available; wall clock stays un-synced");
        return;
    }

    uint64_t unixTimeSeconds =
        static_cast<uint64_t>(lastKnownGoodChipEpoch.count()) + static_cast<uint64_t>(kChipEpochSecondsSinceUnixEpoch);
    if (unixTimeSeconds > UINT32_MAX)
    {
        ChipLogError(DeviceLayer, "Last Known Good UTC Time is out of Unix epoch range");
        return;
    }

    err = System::Clock::RestoreRealTime(System::Clock::Seconds32(static_cast<uint32_t>(unixTimeSeconds)));
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to restore wall clock: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void SilabsTimeSyncDelegate::RefreshLastKnownGoodTime()
{
    System::Clock::Microseconds64 approximateTime;
    RealTimeSource source = RealTimeSource::kUnset;
    VerifyOrReturn(System::Clock::GetApproximateRealTime(approximateTime, source) == CHIP_NO_ERROR);

    // Only a synced clock is worth persisting: writing back a restored value
    // would just rewrite what we read at boot.
    VerifyOrReturn(source == RealTimeSource::kSynced);

    uint64_t chipEpochMicros = 0;
    VerifyOrReturn(UnixEpochToChipEpochMicros(approximateTime.count(), chipEpochMicros));

    const uint64_t chipEpochSeconds = chipEpochMicros / kMicrosecondsPerSecond;
    VerifyOrReturn(chipEpochSeconds <= UINT32_MAX);

    CHIP_ERROR err = Server::GetInstance().GetFabricTable().SetLastKnownGoodChipEpochTime(
        System::Clock::Seconds32(static_cast<uint32_t>(chipEpochSeconds)));
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to refresh Last Known Good UTC Time: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

void SilabsTimeSyncDelegate::RequestTimeSync()
{
    VerifyOrReturn(mCompletion != nullptr);
    VerifyOrReturn(!IsSynced());

    // Reporting a non-None source together with NoTimeGranularity tells the
    // cluster that this delegate could not supply a time, which makes it run
    // the remaining sources, starting with the TrustedTimeSource.
    mCompletion->mCall(mCompletion->mContext, TimeSync::TimeSourceEnum::kUnknown, TimeSync::GranularityEnum::kNoTimeGranularity);
}

void SilabsTimeSyncDelegate::ScheduleRetry()
{
    VerifyOrReturn(!IsSynced());
    VerifyOrReturn(!mRetryScheduled);

    if (mRetryInterval.count() == 0)
    {
        mRetryInterval = kRetryInitial;
    }
    else if (mRetryInterval < kRetryMax)
    {
        const uint32_t doubled = mRetryInterval.count() * 2;
        mRetryInterval         = System::Clock::Seconds32(std::min(doubled, kRetryMax.count()));
    }

    CHIP_ERROR err = SystemLayer().StartTimer(mRetryInterval, OnRetryTimer, this);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to schedule time sync retry: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    mRetryScheduled = true;
}

void SilabsTimeSyncDelegate::CancelRetry()
{
    VerifyOrReturn(mRetryScheduled);
    SystemLayer().CancelTimer(OnRetryTimer, this);
    mRetryScheduled = false;
}

void SilabsTimeSyncDelegate::ScheduleLastKnownGoodTimeRefresh()
{
    VerifyOrReturn(!mRefreshScheduled);

    CHIP_ERROR err = SystemLayer().StartTimer(kLastKnownGoodTimeRefresh, OnLastKnownGoodTimeTimer, this);
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to schedule Last Known Good UTC Time refresh: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    mRefreshScheduled = true;
}

void SilabsTimeSyncDelegate::OnRetryTimer(System::Layer * layer, void * context)
{
    auto * self           = static_cast<SilabsTimeSyncDelegate *>(context);
    self->mRetryScheduled = false;
    self->RequestTimeSync();
}

void SilabsTimeSyncDelegate::OnLastKnownGoodTimeTimer(System::Layer * layer, void * context)
{
    auto * self             = static_cast<SilabsTimeSyncDelegate *>(context);
    self->mRefreshScheduled = false;
    self->RefreshLastKnownGoodTime();
    self->ScheduleLastKnownGoodTimeRefresh();
}

void SilabsTimeSyncDelegate::OnPlatformEvent(const ChipDeviceEvent * event, intptr_t argument)
{
    auto * self = reinterpret_cast<SilabsTimeSyncDelegate *>(argument);
    VerifyOrReturn(self != nullptr && !self->IsSynced());

    switch (event->Type)
    {
    case DeviceEventType::kThreadConnectivityChange:
        if (event->ThreadConnectivityChange.Result == kConnectivity_Established)
        {
            self->CancelRetry();
            self->RequestTimeSync();
        }
        break;
    case DeviceEventType::kInternetConnectivityChange:
        if (event->InternetConnectivityChange.IPv6 == kConnectivity_Established)
        {
            self->CancelRetry();
            self->RequestTimeSync();
        }
        break;
    default:
        break;
    }
}

} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
