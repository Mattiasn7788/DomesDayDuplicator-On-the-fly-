#pragma once

#include <cstdint>

namespace DddUsbProtocol
{
    enum class DeviceProtocol
    {
        Legacy,
        Version1,
        Unsupported,
    };

    // The identifiers used by the original firmware.
    inline constexpr uint16_t LegacyVendorId = 0x1D50;
    inline constexpr uint16_t LegacyProductId = 0x603B;

    // The pid.codes identifiers used by firmware 3.1 and later protocol-v1 builds.
    inline constexpr uint16_t CurrentVendorId = 0x1209;
    inline constexpr uint16_t CurrentProductId = 0x2347;
    inline constexpr uint8_t SupportedProtocolVersion = 1;

    // Vendor-specific requests shared with the device firmware.
    inline constexpr uint8_t CollectionRequest = 0xB5;
    inline constexpr uint8_t LegacyConfigurationRequest = 0xB6;
    inline constexpr uint8_t RegisterReadRequest = 0xB7;
    inline constexpr uint8_t RegisterWriteRequest = 0xB8;
    inline constexpr uint8_t IdentityRegister = 0x00;
    inline constexpr uint8_t IdentityLength = 12;
    inline constexpr uint8_t IdentityValue = 0x44;
    inline constexpr uint8_t RegisterMapVersionRegister = 0x01;
    inline constexpr uint8_t SupportedRegisterMapVersion = 2;
    inline constexpr uint8_t BuildFlagsRegister = 0x02;
    inline constexpr uint8_t CommitRegister = 0x03;
    inline constexpr uint8_t CommitLength = 8;
    inline constexpr uint8_t BuildDirtyFlag = 0x01;
    inline constexpr uint8_t BuildCommitFlag = 0x02;
    inline constexpr uint8_t ImageRoleRegister = 0x0B;
    inline constexpr uint8_t ApplicationImageRole = 1;
    inline constexpr uint8_t TestModeRegister = 0x10;
    inline constexpr uint8_t DecimationRegister = 0x12;
    inline constexpr uint8_t FullRateDecimation = 1;
    inline constexpr uint8_t HalfRateDecimation = 2;
    inline constexpr uint32_t ConverterSampleRateInHz = 40000000;

    constexpr bool IsLegacyDevice(uint16_t vendorId, uint16_t productId)
    {
        return vendorId == LegacyVendorId && productId == LegacyProductId;
    }

    constexpr bool IsCurrentDevice(uint16_t vendorId, uint16_t productId)
    {
        return vendorId == CurrentVendorId && productId == CurrentProductId;
    }

    constexpr bool IsKnownDevice(uint16_t vendorId, uint16_t productId)
    {
        return IsLegacyDevice(vendorId, productId) || IsCurrentDevice(vendorId, productId);
    }

    // A known DDD identifier in the settings selects both official generations. A custom
    // identifier remains an exact override, preserving the existing advanced setting.
    constexpr bool MatchesConfiguredDevice(uint16_t configuredVendorId, uint16_t configuredProductId,
        uint16_t candidateVendorId, uint16_t candidateProductId)
    {
        if (IsKnownDevice(configuredVendorId, configuredProductId))
        {
            return IsKnownDevice(candidateVendorId, candidateProductId);
        }
        return candidateVendorId == configuredVendorId && candidateProductId == configuredProductId;
    }

    // Firmware 3.1 carries its host-protocol version in the high byte of bcdDevice. Legacy
    // firmware predates that field, while a custom VID/PID may use either protocol.
    constexpr DeviceProtocol ClassifyDevice(uint16_t vendorId, uint16_t productId, uint16_t bcdDevice)
    {
        if (IsLegacyDevice(vendorId, productId))
        {
            return DeviceProtocol::Legacy;
        }

        const uint8_t protocolVersion = static_cast<uint8_t>(bcdDevice >> 8);
        if (protocolVersion == SupportedProtocolVersion)
        {
            return DeviceProtocol::Version1;
        }

        if (IsCurrentDevice(vendorId, productId) || protocolVersion != 0)
        {
            return DeviceProtocol::Unsupported;
        }
        return DeviceProtocol::Legacy;
    }

    constexpr uint16_t MakeRegisterWrite(uint8_t address, uint8_t value)
    {
        return static_cast<uint16_t>((static_cast<uint16_t>(address) << 8) | value);
    }

    constexpr uint16_t MakeTestModeWrite(bool testMode)
    {
        return MakeRegisterWrite(TestModeRegister, testMode ? 1 : 0);
    }

    constexpr bool IsSupportedDecimationFactor(uint8_t factor)
    {
        return factor == FullRateDecimation || factor == HalfRateDecimation;
    }

    constexpr uint16_t MakeDecimationWrite(uint8_t factor)
    {
        return MakeRegisterWrite(DecimationRegister, factor);
    }

    constexpr uint32_t SampleRateInHzForDecimation(uint8_t factor)
    {
        return IsSupportedDecimationFactor(factor) ? ConverterSampleRateInHz / factor : 0;
    }

    // FLAC cannot represent a 20/40 MHz rate in its stream header. The DDD/ld-decode
    // convention is a 20/40 kHz label plus DDD_SAMPLE_RATE_HZ carrying the real rate.
    constexpr uint32_t FlacSampleRateLabelForDecimation(uint8_t factor)
    {
        return SampleRateInHzForDecimation(factor) / 1000;
    }

    constexpr bool IsSupportedApplicationIdentity(uint8_t identity, uint8_t registerMapVersion,
        uint8_t imageRole)
    {
        return identity == IdentityValue && registerMapVersion == SupportedRegisterMapVersion &&
            imageRole == ApplicationImageRole;
    }

    static_assert(MatchesConfiguredDevice(LegacyVendorId, LegacyProductId, CurrentVendorId, CurrentProductId));
    static_assert(MatchesConfiguredDevice(CurrentVendorId, CurrentProductId, LegacyVendorId, LegacyProductId));
    static_assert(!MatchesConfiguredDevice(0x1234, 0x5678, CurrentVendorId, CurrentProductId));
    static_assert(ClassifyDevice(LegacyVendorId, LegacyProductId, 0x0000) == DeviceProtocol::Legacy);
    static_assert(ClassifyDevice(CurrentVendorId, CurrentProductId, 0x0100) == DeviceProtocol::Version1);
    static_assert(ClassifyDevice(CurrentVendorId, CurrentProductId, 0x0200) == DeviceProtocol::Unsupported);
    static_assert(MakeTestModeWrite(false) == 0x1000);
    static_assert(MakeTestModeWrite(true) == 0x1001);
    static_assert(MakeDecimationWrite(FullRateDecimation) == 0x1201);
    static_assert(MakeDecimationWrite(HalfRateDecimation) == 0x1202);
    static_assert(SampleRateInHzForDecimation(FullRateDecimation) == 40000000);
    static_assert(SampleRateInHzForDecimation(HalfRateDecimation) == 20000000);
    static_assert(FlacSampleRateLabelForDecimation(FullRateDecimation) == 40000);
    static_assert(FlacSampleRateLabelForDecimation(HalfRateDecimation) == 20000);
    static_assert(IsSupportedApplicationIdentity(0x44, 2, 1));
    static_assert(!IsSupportedApplicationIdentity(0x44, 2, 0));
}
