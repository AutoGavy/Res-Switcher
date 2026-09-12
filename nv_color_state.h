#pragma once

#include <cstdint>
#include <string>

struct NvidiaColorState {
    std::uint8_t colorFormat = 0;
    std::uint8_t colorimetry = 0;
    std::uint8_t dynamicRange = 0;
    std::int32_t bpc = 0;
    std::int32_t desktopDepth = 0;
    std::uint32_t apiVersion = 0;
    bool hasDesktopDepth = false;
};

class NvidiaColorControl {
public:
    static bool ReadPrimary(
        NvidiaColorState& state,
        std::wstring& error,
        unsigned int attempts = 1);

    static bool IsSupportedOnPrimary(
        const NvidiaColorState& state,
        std::wstring& error,
        unsigned int attempts = 1);

    static bool ApplyToPrimary(
        const NvidiaColorState& state,
        std::wstring& error,
        unsigned int attempts = 1);

    static bool Equivalent(
        const NvidiaColorState& left,
        const NvidiaColorState& right);

    static std::wstring Describe(const NvidiaColorState& state);
};
