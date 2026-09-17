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

#include <lib/core/CHIPError.h>

namespace chip {
namespace System {
namespace Clock {

/**
 * Initializes the platform real time clock.
 *
 * Restores the last persisted wall clock time, if any, so that the device boots
 * with a plausible date rather than the Matter epoch. If no time has ever been
 * persisted, the real time clock is left un-synced and GetClock_RealTime() will
 * report CHIP_ERROR_REAL_TIME_NOT_SYNCED until it is set (for example by the
 * Time Synchronization cluster SetUTCTime command during commissioning).
 */
CHIP_ERROR InitClock_RealTime();

/**
 * Persists the current wall clock time so that it can be restored on the next
 * boot by InitClock_RealTime().
 *
 * SetClock_RealTime() already persists on every successful update, which covers
 * the Time Synchronization cluster. Applications that stay powered for long
 * periods between time updates may additionally call this periodically (for
 * example hourly) to bound how far the restored time lags reality after an
 * unexpected reset.
 *
 * This performs a non-volatile write; do not call it at high frequency.
 *
 * @return CHIP_NO_ERROR on success, CHIP_ERROR_REAL_TIME_NOT_SYNCED if the
 *         clock has never been set, or a platform error on write failure.
 */
CHIP_ERROR PersistClock_RealTime();

} // namespace Clock
} // namespace System
} // namespace chip
