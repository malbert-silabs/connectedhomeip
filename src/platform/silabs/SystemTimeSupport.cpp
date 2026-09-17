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

/**
 *    @file
 *          Implementation of the CHIP System Layer clock functions for Silicon
 *          Labs platforms, backed by the Sleeptimer service (RTCC / SYSRTC /
 *          BURTC depending on board configuration) rather than the FreeRTOS
 *          scheduler tick.
 *
 *          Compared to the generic src/platform/FreeRTOS implementation this
 *          provides:
 *            - A monotonic time base read directly from the low frequency RTC
 *              counter, so it is unaffected by tickless-idle tick accounting and
 *              keeps advancing correctly across EM2/EM3 sleep.
 *            - A working GetClock_RealTime(). The FreeRTOS implementation
 *              unconditionally returns CHIP_ERROR_UNSUPPORTED_CHIP_FEATURE
 *              (see project-chip/connectedhomeip#19081), which makes the Time
 *              Synchronization cluster report a null UTCTime attribute even
 *              after a successful SetUTCTime.
 *            - Honest "not synced" reporting, so callers can distinguish
 *              "no wall clock time" from "wall clock time is the Matter epoch".
 *            - Persistence of the wall clock across reset, so the device does
 *              not fall back to 2000-01-01 on every boot.
 */

/* this file behaves like a config.h, comes first */
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <platform/silabs/SilabsConfig.h>
#include <platform/silabs/SystemTimeSupport.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/TimeUtils.h>

#include "sl_sleeptimer.h"

namespace chip {
namespace System {
namespace Clock {

namespace Internal {
ClockImpl gClockImpl;
} // namespace Internal

namespace {

using chip::DeviceLayer::Internal::SilabsConfig;

/**
 * Unix epoch seconds corresponding to the wall clock time at the instant the
 * monotonic counter read zero. Real time is therefore
 * sBootTimeUS + monotonic. A value of zero means "never set".
 *
 * Stored in microseconds to avoid rounding on every read.
 */
uint64_t sBootTimeUS = 0;

/**
 * Converts a Sleeptimer tick count to microseconds without overflowing.
 *
 * A naive (ticks * kMicrosecondsPerSecond) overflows uint64 after roughly 18
 * years at the typical 32768 Hz LFXO rate, so the quotient and remainder are
 * scaled separately.
 */
uint64_t TicksToMicroseconds(uint64_t ticks, uint32_t frequency)
{
    VerifyOrReturnValue(frequency != 0, 0);

    const uint64_t seconds   = ticks / frequency;
    const uint64_t remainder = ticks % frequency;

    return (seconds * kMicrosecondsPerSecond) + ((remainder * kMicrosecondsPerSecond) / frequency);
}

uint64_t GetMonotonicMicroseconds()
{
    // Idempotent: returns early if the platform already initialized Sleeptimer.
    VerifyOrReturnValue(sl_sleeptimer_init() == SL_STATUS_OK, 0);

    return TicksToMicroseconds(sl_sleeptimer_get_tick_count64(), sl_sleeptimer_get_timer_frequency());
}

/**
 * Returns true if the given Unix time is recent enough to be plausible.
 *
 * Anything at or below the threshold (2000-01-01) is treated as "no real time",
 * matching the convention used by the other platform implementations.
 */
bool IsPlausibleRealTime(uint64_t unixTimeUS)
{
    return unixTimeUS > (static_cast<uint64_t>(CHIP_SYSTEM_CONFIG_VALID_REAL_TIME_THRESHOLD) * kMicrosecondsPerSecond);
}

} // unnamed namespace

Microseconds64 ClockImpl::GetMonotonicMicroseconds64()
{
    return Microseconds64(GetMonotonicMicroseconds());
}

Milliseconds64 ClockImpl::GetMonotonicMilliseconds64()
{
    return std::chrono::duration_cast<Milliseconds64>(GetMonotonicMicroseconds64());
}

uint64_t GetClock_Monotonic()
{
    return GetMonotonicMicroseconds();
}

uint64_t GetClock_MonotonicMS()
{
    return GetMonotonicMicroseconds() / kMicrosecondsPerMillisecond;
}

uint64_t GetClock_MonotonicHiRes()
{
    return GetMonotonicMicroseconds();
}

CHIP_ERROR ClockImpl::GetClock_RealTime(Microseconds64 & aCurTime)
{
    VerifyOrReturnError(sBootTimeUS != 0, CHIP_ERROR_REAL_TIME_NOT_SYNCED);

    const uint64_t currentTimeUS = sBootTimeUS + GetMonotonicMicroseconds();

    // Guards against a persisted value that is somehow below the threshold.
    VerifyOrReturnError(IsPlausibleRealTime(currentTimeUS), CHIP_ERROR_REAL_TIME_NOT_SYNCED);

    aCurTime = Microseconds64(currentTimeUS);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ClockImpl::GetClock_RealTimeMS(Milliseconds64 & aCurTime)
{
    Microseconds64 currentTimeUS;
    ReturnErrorOnFailure(GetClock_RealTime(currentTimeUS));

    aCurTime = std::chrono::duration_cast<Milliseconds64>(currentTimeUS);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ClockImpl::SetClock_RealTime(Microseconds64 aNewCurTime)
{
    VerifyOrReturnError(IsPlausibleRealTime(aNewCurTime.count()), CHIP_ERROR_INVALID_TIME);

    const uint64_t monotonicUS = GetMonotonicMicroseconds();
    VerifyOrReturnError(aNewCurTime.count() > monotonicUS, CHIP_ERROR_INVALID_TIME);

    sBootTimeUS = aNewCurTime.count() - monotonicUS;

    // Best effort: losing persistence must not fail the time update itself,
    // since the in-memory clock is now correct either way.
    CHIP_ERROR err = PersistClock_RealTime();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Failed to persist real time: %" CHIP_ERROR_FORMAT, err.Format());
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR PersistClock_RealTime()
{
    Microseconds64 currentTimeUS;
    ReturnErrorOnFailure(Internal::gClockImpl.GetClock_RealTime(currentTimeUS));

    // Seconds resolution is sufficient here and keeps the stored value small.
    // The restored time is inherently approximate, so sub-second precision
    // would be misleading.
    return SilabsConfig::WriteConfigValue(SilabsConfig::kConfigKey_LastKnownUnixTime,
                                          currentTimeUS.count() / kMicrosecondsPerSecond);
}

CHIP_ERROR InitClock_RealTime()
{
    // Ensure the RTC based counter is running before anything reads the clock.
    VerifyOrReturnError(sl_sleeptimer_init() == SL_STATUS_OK, CHIP_ERROR_INTERNAL);

    sBootTimeUS = 0;

    uint64_t persistedUnixTimeS = 0;
    CHIP_ERROR err              = SilabsConfig::ReadConfigValue(SilabsConfig::kConfigKey_LastKnownUnixTime, persistedUnixTimeS);
    if (err != CHIP_NO_ERROR)
    {
        // Nothing stored yet (first boot or post factory reset). Deliberately
        // leave the clock un-synced instead of seeding it with the Matter epoch:
        // reporting "no time" is more truthful and lets the Time Synchronization
        // cluster and the commissioner supply a real time.
        ChipLogProgress(DeviceLayer, "No persisted real time available; wall clock is un-synced");
        return CHIP_NO_ERROR;
    }

    const uint64_t persistedUnixTimeUS = persistedUnixTimeS * kMicrosecondsPerSecond;
    if (!IsPlausibleRealTime(persistedUnixTimeUS))
    {
        ChipLogError(DeviceLayer, "Persisted real time is implausible; wall clock is un-synced");
        return CHIP_NO_ERROR;
    }

    const uint64_t monotonicUS = GetMonotonicMicroseconds();
    VerifyOrReturnError(persistedUnixTimeUS > monotonicUS, CHIP_NO_ERROR);

    sBootTimeUS = persistedUnixTimeUS - monotonicUS;

    ChipLogProgress(DeviceLayer, "Restored real time from storage: %" PRIu64 " (Unix seconds)", persistedUnixTimeS);

    return CHIP_NO_ERROR;
}

} // namespace Clock
} // namespace System
} // namespace chip
