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

#include <cinttypes>
#include <cstring>

#include <platform/silabs/zephyr/SilabsFactoryDataProvider.h>

#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>
#include <sl_psa_values.h>

namespace chip {
namespace DeviceLayer {
namespace Silabs {
namespace Provision {

namespace {
constexpr char kMissing[] = "(missing)";
} // namespace

void SilabsFactoryDataProvider::LogProvisionedData()
{
    char buf[Storage::kSerialNumberLengthMax + 1] = { 0 };

    // Use ChipLogError so this dump is visible during early boot when Init() may fail
    // immediately afterward (e.g. missing Secure Vault DAC key). ChipLogProgress maps to
    // Zephyr LOG_INF and is easy to miss alongside the surrounding error lines.
    ChipLogError(DeviceLayer, "Factory partition provisioning data:");

    if (CHIP_NO_ERROR == GetSerialNumber(buf, sizeof(buf)))
    {
        ChipLogError(DeviceLayer, "  serialNumber: %s", buf);
    }
    else
    {
        ChipLogError(DeviceLayer, "  serialNumber: %s", kMissing);
    }

    uint16_t vendorId = 0;
    if (CHIP_NO_ERROR == GetVendorId(vendorId))
    {
        ChipLogError(DeviceLayer, "  vendorId: 0x%04X", vendorId);
    }
    else
    {
        ChipLogError(DeviceLayer, "  vendorId: %s", kMissing);
    }

    buf[0] = '\0';
    if (CHIP_NO_ERROR == GetVendorName(buf, sizeof(buf)))
    {
        ChipLogError(DeviceLayer, "  vendorName: %s", buf);
    }
    else
    {
        ChipLogError(DeviceLayer, "  vendorName: %s", kMissing);
    }

    uint16_t productId = 0;
    if (CHIP_NO_ERROR == GetProductId(productId))
    {
        ChipLogError(DeviceLayer, "  productId: 0x%04X", productId);
    }
    else
    {
        ChipLogError(DeviceLayer, "  productId: %s", kMissing);
    }

    buf[0] = '\0';
    if (CHIP_NO_ERROR == GetProductName(buf, sizeof(buf)))
    {
        ChipLogError(DeviceLayer, "  productName: %s", buf);
    }
    else
    {
        ChipLogError(DeviceLayer, "  productName: %s", kMissing);
    }

    uint16_t hwVersion = 0;
    if (CHIP_NO_ERROR == GetHardwareVersion(hwVersion))
    {
        ChipLogError(DeviceLayer, "  hardwareVersion: %u", hwVersion);
    }
    else
    {
        ChipLogError(DeviceLayer, "  hardwareVersion: %s", kMissing);
    }

    buf[0] = '\0';
    if (CHIP_NO_ERROR == GetHardwareVersionString(buf, sizeof(buf)))
    {
        ChipLogError(DeviceLayer, "  hardwareVersionString: %s", buf);
    }
    else
    {
        ChipLogError(DeviceLayer, "  hardwareVersionString: %s", kMissing);
    }

    uint16_t discriminator = 0;
    if (CHIP_NO_ERROR == GetSetupDiscriminator(discriminator))
    {
        ChipLogError(DeviceLayer, "  setupDiscriminator: %u", discriminator);
    }
    else
    {
        ChipLogError(DeviceLayer, "  setupDiscriminator: %s", kMissing);
    }

    uint32_t passcode = 0;
    if (CHIP_NO_ERROR == GetSetupPasscode(passcode))
    {
        ChipLogError(DeviceLayer, "  setupPasscode: %u", passcode);
    }
    else
    {
        ChipLogError(DeviceLayer, "  setupPasscode: %s", kMissing);
    }

    uint32_t spake2pIterations = 0;
    if (CHIP_NO_ERROR == GetSpake2pIterationCount(spake2pIterations))
    {
        ChipLogError(DeviceLayer, "  spake2pIterationCount: %u", spake2pIterations);
    }
    else
    {
        ChipLogError(DeviceLayer, "  spake2pIterationCount: %s", kMissing);
    }

    uint32_t credsAddr = 0;
    if (CHIP_NO_ERROR == GetCredentialsBaseAddress(credsAddr))
    {
        ChipLogError(DeviceLayer, "  credsBaseAddress: 0x%08" PRIX32, credsAddr);
    }
    else
    {
        ChipLogError(DeviceLayer, "  credsBaseAddress: %s", kMissing);
    }

    uint8_t certBuf[Storage::kCertificationSizeMax] = { 0 };
    MutableByteSpan cdSpan(certBuf);
    if (CHIP_NO_ERROR == GetCertificationDeclaration(cdSpan))
    {
        ChipLogError(DeviceLayer, "  certificationDeclaration: %u bytes", static_cast<unsigned>(cdSpan.size()));
    }
    else
    {
        ChipLogError(DeviceLayer, "  certificationDeclaration: %s", kMissing);
    }

    certBuf[0] = '\0';
    MutableByteSpan paiSpan(certBuf);
    if (CHIP_NO_ERROR == GetProductAttestationIntermediateCert(paiSpan))
    {
        ChipLogError(DeviceLayer, "  paiCert: %u bytes", static_cast<unsigned>(paiSpan.size()));
    }
    else
    {
        ChipLogError(DeviceLayer, "  paiCert: %s", kMissing);
    }

    certBuf[0] = '\0';
    MutableByteSpan dacSpan(certBuf);
    if (CHIP_NO_ERROR == GetDeviceAttestationCert(dacSpan))
    {
        ChipLogError(DeviceLayer, "  dacCert: %u bytes", static_cast<unsigned>(dacSpan.size()));
    }
    else
    {
        ChipLogError(DeviceLayer, "  dacCert: %s", kMissing);
    }
}

CHIP_ERROR SilabsFactoryDataProvider::EnsureDacKeyInSecureVault()
{
    psa_key_attributes_t existingAttributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status                     = psa_get_key_attributes(kSilabsDacPsaKeyId, &existingAttributes);
    if (status == PSA_SUCCESS)
    {
        psa_reset_key_attributes(&existingAttributes);
        return CHIP_NO_ERROR;
    }
    psa_reset_key_attributes(&existingAttributes);

    ChipLogError(DeviceLayer,
                 "DAC PSA key id %u missing (status=%d). Run CSR provisioning before booting Zephyr; do not generate a "
                 "substitute key here.",
                 static_cast<unsigned>(kSilabsDacPsaKeyId), static_cast<int>(status));
    return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
}

CHIP_ERROR SilabsFactoryDataProvider::Init()
{
    ReturnErrorOnFailure(Initialize());
    LogProvisionedData();
    ReturnErrorOnFailure(EnsureDacKeyInSecureVault());
    mInitialized = true;
    return CHIP_NO_ERROR;
}

CHIP_ERROR SilabsFactoryDataProvider::SignWithDeviceAttestationKey(const ByteSpan & message, MutableByteSpan & signature)
{
    VerifyOrReturnError(mInitialized, CHIP_ERROR_INCORRECT_STATE);

    uint8_t sigBuffer[64] = { 0 };
    size_t sigLen         = sizeof(sigBuffer);

    psa_status_t status =
        psa_sign_message(kSilabsDacPsaKeyId, PSA_ALG_ECDSA(PSA_ALG_SHA_256), message.data(), message.size(), sigBuffer, sigLen, &sigLen);
    VerifyOrReturnError(status == PSA_SUCCESS, CHIP_ERROR_INTERNAL);
    VerifyOrReturnError(sigLen == 64, CHIP_ERROR_INTERNAL);

    return CopySpanToMutableSpan(ByteSpan(sigBuffer, sigLen), signature);
}

CHIP_ERROR SilabsFactoryDataProvider::GetDeviceAttestationCSR(uint16_t vid, uint16_t pid, const CharSpan & cn, MutableCharSpan & csr)
{
    ReturnErrorOnFailure(Init());

    VerifyOrReturnError(csr.data() != nullptr, CHIP_ERROR_INVALID_ARGUMENT);
    VerifyOrReturnError(csr.size() >= 512, CHIP_ERROR_BUFFER_TOO_SMALL);

    mbedtls_pk_context keyCtx;
    mbedtls_x509write_csr csrCtx;
    mbedtls_x509write_csr_init(&csrCtx);
    mbedtls_pk_init(&keyCtx);

    const char * commonName = (cn.size() > 0 && cn.data()[0] != '\0') ? cn.data() : "Matter Device";
    char subjectName[128]    = { 0 };
    snprintf(subjectName, sizeof(subjectName), "CN=%s, 1.3.6.1.4.1.37244.2.1=%04X, 1.3.6.1.4.1.37244.2.2=%04X", commonName, vid, pid);

    CHIP_ERROR result = CHIP_NO_ERROR;
    int err           = mbedtls_x509write_csr_set_subject_name(&csrCtx, subjectName);
    VerifyOrExit(err == 0, result = CHIP_ERROR_INTERNAL);

    mbedtls_x509write_csr_set_md_alg(&csrCtx, MBEDTLS_MD_SHA256);

    err = mbedtls_pk_wrap_psa(&keyCtx, kSilabsDacPsaKeyId);
    VerifyOrExit(err == 0, result = CHIP_ERROR_INTERNAL);

    mbedtls_x509write_csr_set_key(&csrCtx, &keyCtx);

    err = mbedtls_x509write_csr_pem(&csrCtx, reinterpret_cast<unsigned char *>(csr.data()), csr.size());
    VerifyOrExit(err == 0, result = CHIP_ERROR_INTERNAL);

    csr.reduce_size(strlen(reinterpret_cast<char *>(csr.data())) + 1);

exit:
    mbedtls_x509write_csr_free(&csrCtx);
    mbedtls_pk_free(&keyCtx);
    return result;
}

} // namespace Provision
} // namespace Silabs
} // namespace DeviceLayer
} // namespace chip
