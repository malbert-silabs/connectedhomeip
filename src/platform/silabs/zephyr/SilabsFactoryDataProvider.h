/*
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

#include <headers/ProvisionStorage.h>
#include <lib/core/CHIPError.h>
#include <psa/crypto.h>

namespace chip {
namespace DeviceLayer {
namespace Silabs {
namespace Provision {

/**
 * Factory data provider for Silabs Zephyr builds using CONFIG_CHIP_SILABS_SECURE_VAULT_DAC.
 * The DAC private key is generated inside the Secure Vault and never stored in flash.
 */
class SilabsFactoryDataProvider : public Storage
{
public:
    static constexpr psa_key_id_t kSilabsDacPsaKeyId = static_cast<psa_key_id_t>(2);

    CHIP_ERROR Init();
    CHIP_ERROR SignWithDeviceAttestationKey(const ByteSpan & message, MutableByteSpan & signature) override;
    CHIP_ERROR GetDeviceAttestationCSR(uint16_t vid, uint16_t pid, const CharSpan & cn, MutableCharSpan & csr);

private:
    CHIP_ERROR EnsureDacKeyInSecureVault();
    void LogProvisionedData();
    bool mInitialized = false;
};

} // namespace Provision
} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
