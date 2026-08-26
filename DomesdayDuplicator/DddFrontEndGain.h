/************************************************************************

    DddFrontEndGain.h

    Domesday Duplicator SW401 front-end gain declaration helpers

************************************************************************/
#pragma once

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

// SW401 is a mechanical four-way DIP switch.  It is not connected to the
// FPGA or FX3, so this namespace only describes a user-declared switch
// position; none of these helpers can change or detect the hardware gain.
namespace DddFrontEndGain
{
constexpr std::uint8_t UndeclaredSwitchPattern = 0;
constexpr std::uint8_t MaximumSwitchPattern = 0x0F;

// Switch 1 is the most-significant bit.  The four switched feedback
// resistors are connected in parallel against the fixed 200 ohm leg.
constexpr double SwitchResistanceOhms[] = {1500.0, 1000.0, 680.0, 560.0};
constexpr std::size_t SwitchCount =
        sizeof(SwitchResistanceOhms) / sizeof(SwitchResistanceOhms[0]);
constexpr double FixedGainResistanceOhms = 200.0;
constexpr double AdcFullScaleMillivoltsPeakToPeak = 2000.0;

inline bool IsDeclared(int switchPattern)
{
    return switchPattern >= 1 && switchPattern <= MaximumSwitchPattern;
}

// Invalid values from an edited or newer settings file safely become the
// undeclared state rather than an authoritative-looking calibration.
inline std::uint8_t NormalizeSwitchPattern(int switchPattern)
{
    return IsDeclared(switchPattern)
            ? static_cast<std::uint8_t>(switchPattern)
            : UndeclaredSwitchPattern;
}

// Four digits in the physical switch order, with 1 meaning ON/closed.
// The undeclared state has no pattern and therefore returns an empty string.
inline std::string SwitchPatternString(int switchPattern)
{
    const std::uint8_t pattern = NormalizeSwitchPattern(switchPattern);
    if (pattern == UndeclaredSwitchPattern)
        return std::string();

    std::string text;
    text.reserve(SwitchCount);
    for (std::size_t index = 0; index < SwitchCount; ++index) {
        const unsigned bit = 1U << (SwitchCount - 1 - index);
        text += ((pattern & bit) != 0) ? '1' : '0';
    }
    return text;
}

inline double FeedbackResistanceOhms(int switchPattern)
{
    const std::uint8_t pattern = NormalizeSwitchPattern(switchPattern);
    if (pattern == UndeclaredSwitchPattern)
        return 0.0;

    double conductance = 0.0;
    for (std::size_t index = 0; index < SwitchCount; ++index) {
        const unsigned bit = 1U << (SwitchCount - 1 - index);
        if ((pattern & bit) != 0)
            conductance += 1.0 / SwitchResistanceOhms[index];
    }
    return 1.0 / conductance;
}

inline double Gain(int switchPattern)
{
    if (!IsDeclared(switchPattern))
        return 0.0;
    return 1.0 + (FeedbackResistanceOhms(switchPattern) / FixedGainResistanceOhms);
}

inline double FullScaleInputMillivoltsPeakToPeak(int switchPattern)
{
    const double gain = Gain(switchPattern);
    return gain > 0.0 ? AdcFullScaleMillivoltsPeakToPeak / gain : 0.0;
}

// The same human-readable declaration used by the firmware 3.1 capture
// application for provenance. Empty means no physical switch position was
// declared and therefore no calibration claim should be written.
inline std::string Description(int switchPattern)
{
    if (!IsDeclared(switchPattern))
        return std::string();

    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Switches " << SwitchPatternString(switchPattern) << "  (\xC3\x97"
         << std::fixed << std::setprecision(2) << Gain(switchPattern) << ", "
         << std::setprecision(0) << FullScaleInputMillivoltsPeakToPeak(switchPattern)
         << " mV p-p full scale)";
    return text.str();
}
} // namespace DddFrontEndGain
