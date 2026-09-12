#include <windows.h>

#include "nv_color_state.h"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Resolution {
    DWORD width;
    DWORD height;
};

constexpr Resolution kDefaultLowerResolution{1720, 1080};
constexpr Resolution k1050Resolution{1680, 1050};
constexpr Resolution k1080Resolution{1920, 1080};
constexpr Resolution k1440Resolution{2560, 1440};
constexpr Resolution kUltraHdResolution{3840, 2160};

constexpr DWORD kDpiScaleValues[] = {
    100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500
};

constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kGetDpiScaleType =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kSetDpiScaleType =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);

struct DisplaySource {
    LUID adapterId;
    UINT32 sourceId;
};

struct DpiScaleInfo {
    LONG minScaleRel;
    LONG currentScaleRel;
    LONG maxScaleRel;
};

struct DpiScaleGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    LONG minScaleRel;
    LONG currentScaleRel;
    LONG maxScaleRel;
};

struct DpiScaleSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    LONG scaleRel;
};

bool HasResolution(const DEVMODEW& mode, const Resolution resolution) {
    return mode.dmPelsWidth == resolution.width &&
           mode.dmPelsHeight == resolution.height;
}

std::optional<DISPLAY_DEVICEW> FindPrimaryDisplay() {
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);

        if (!EnumDisplayDevicesW(nullptr, index, &display, 0)) {
            return std::nullopt;
        }

        const DWORD requiredFlags = DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_PRIMARY_DEVICE;
        if ((display.StateFlags & requiredFlags) == requiredFlags) {
            return display;
        }
    }
}

std::optional<DEVMODEW> ReadCurrentMode(const wchar_t* deviceName) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);

    if (!EnumDisplaySettingsExW(deviceName, ENUM_CURRENT_SETTINGS, &mode, 0)) {
        return std::nullopt;
    }

    return mode;
}

std::optional<DisplaySource> FindDisplaySource(const wchar_t* deviceName) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            &modeCount) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[index].sourceInfo.adapterId;
        sourceName.header.id = paths[index].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
            continue;
        }

        if (std::wstring_view{sourceName.viewGdiDeviceName} == deviceName) {
            return DisplaySource{
                paths[index].sourceInfo.adapterId,
                paths[index].sourceInfo.id};
        }
    }

    return std::nullopt;
}

std::optional<DpiScaleInfo> ReadDpiScale(const DisplaySource& source) {
    DpiScaleGet request{};
    request.header.type = kGetDpiScaleType;
    request.header.size = sizeof(request);
    request.header.adapterId = source.adapterId;
    request.header.id = source.sourceId;

    if (DisplayConfigGetDeviceInfo(&request.header) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    return DpiScaleInfo{
        request.minScaleRel,
        request.currentScaleRel,
        request.maxScaleRel};
}

std::optional<LONG> RelativeDpiScaleForPercent(
    const DpiScaleInfo& scale,
    const DWORD targetPercent) {
    int targetIndex = -1;
    for (int index = 0; index < static_cast<int>(std::size(kDpiScaleValues)); ++index) {
        if (kDpiScaleValues[index] == targetPercent) {
            targetIndex = index;
            break;
        }
    }

    if (targetIndex < 0) {
        return std::nullopt;
    }

    // The DisplayConfig API stores the selected scale relative to Windows'
    // recommended scale for this source, not as an absolute percentage.
    const int recommendedIndex = -scale.minScaleRel;
    const LONG relativeScale = targetIndex - recommendedIndex;
    if (relativeScale < scale.minScaleRel ||
        relativeScale > scale.maxScaleRel) {
        return std::nullopt;
    }

    return relativeScale;
}

std::optional<DWORD> DpiPercentForRelativeScale(
    const DpiScaleInfo& scale,
    const LONG relativeScale) {
    const int index = -scale.minScaleRel + relativeScale;
    if (index < 0 || index >= static_cast<int>(std::size(kDpiScaleValues))) {
        return std::nullopt;
    }
    return kDpiScaleValues[index];
}

bool SetDpiScale(const DisplaySource& source, const LONG relativeScale) {
    DpiScaleSet request{};
    request.header.type = kSetDpiScaleType;
    request.header.size = sizeof(request);
    request.header.adapterId = source.adapterId;
    request.header.id = source.sourceId;
    request.scaleRel = relativeScale;
    return DisplayConfigSetDeviceInfo(&request.header) == ERROR_SUCCESS;
}

bool NotifySettingsChange() {
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(
               HWND_BROADCAST,
               WM_SETTINGCHANGE,
               0,
               0,
               SMTO_ABORTIFHUNG | SMTO_BLOCK,
               2000,
               &result) != 0;
}

DWORD TargetDpiPercent(const Resolution resolution) {
    return resolution.height == kUltraHdResolution.height ||
               resolution.height == k1440Resolution.height
        ? 150
        : 125;
}

std::optional<DEVMODEW> FindPreservingTargetMode(
    const wchar_t* deviceName,
    const DEVMODEW& current,
    const Resolution targetResolution) {
    for (DWORD index = 0;; ++index) {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);

        if (!EnumDisplaySettingsExW(deviceName, index, &candidate, 0)) {
            break;
        }

        if (!HasResolution(candidate, targetResolution) ||
            candidate.dmDisplayFrequency != current.dmDisplayFrequency ||
            candidate.dmBitsPerPel != current.dmBitsPerPel ||
            (candidate.dmDisplayFlags & DM_INTERLACED) !=
                (current.dmDisplayFlags & DM_INTERLACED)) {
            continue;
        }

        // EnumDisplaySettings can include modes advertised by the driver that
        // the current connection, color depth, or display topology cannot use.
        if (ChangeDisplaySettingsExW(
                deviceName,
                &candidate,
                nullptr,
                CDS_TEST,
                nullptr) != DISP_CHANGE_SUCCESSFUL) {
            continue;
        }

        return candidate;
    }

    return std::nullopt;
}

bool RestoreOriginalDisplayState(
    const wchar_t* deviceName,
    const DEVMODEW& originalMode,
    const NvidiaColorState& originalColor,
    const DisplaySource& displaySource,
    const LONG originalDpiScale,
    std::wstring& error) {
    DEVMODEW restoreMode = originalMode;
    const LONG restoreResult = ChangeDisplaySettingsExW(
        deviceName,
        &restoreMode,
        nullptr,
        CDS_UPDATEREGISTRY,
        nullptr);
    if (restoreResult != DISP_CHANGE_SUCCESSFUL) {
        error = L"the original display mode could not be restored";
        return false;
    }

    NvidiaColorState restoredColor;
    std::wstring colorError;
    const bool colorAlreadyRestored =
        NvidiaColorControl::ReadPrimary(restoredColor, colorError, 20) &&
        NvidiaColorControl::Equivalent(restoredColor, originalColor);
    if (!colorAlreadyRestored &&
        (!NvidiaColorControl::ApplyToPrimary(originalColor, colorError, 20) ||
         !NvidiaColorControl::ReadPrimary(restoredColor, colorError, 20) ||
         !NvidiaColorControl::Equivalent(restoredColor, originalColor))) {
        error = L"the original resolution was restored, but its NVIDIA color "
                L"settings could not be restored: " + colorError;
        return false;
    }

    if (!SetDpiScale(displaySource, originalDpiScale)) {
        error = L"the original display mode and NVIDIA color settings were "
                L"restored, but the original DPI scaling could not be restored";
        return false;
    }

    return true;
}

const wchar_t* DisplayChangeError(const LONG result) {
    switch (result) {
    case DISP_CHANGE_BADDUALVIEW:
        return L"The requested mode is incompatible with DualView.";
    case DISP_CHANGE_BADFLAGS:
        return L"Invalid display-change flags were supplied.";
    case DISP_CHANGE_BADMODE:
        return L"The graphics mode is not supported.";
    case DISP_CHANGE_BADPARAM:
        return L"An invalid display setting was supplied.";
    case DISP_CHANGE_FAILED:
        return L"The display driver could not apply the graphics mode.";
    case DISP_CHANGE_NOTUPDATED:
        return L"Windows could not save the new mode to the registry.";
    case DISP_CHANGE_RESTART:
        return L"Windows requires a restart before the new mode can take effect.";
    default:
        return L"An unknown display-change error occurred.";
    }
}

void PrintMode(const wchar_t* label, const DEVMODEW& mode) {
    std::wcout << label << mode.dmPelsWidth << L'x' << mode.dmPelsHeight;
    if (mode.dmDisplayFrequency > 1) {
        std::wcout << L" @ " << mode.dmDisplayFrequency << L" Hz";
    }
    std::wcout << L'\n';
}

void WaitWhenOpenedDirectly(const bool shouldWait, const bool success) {
    if (!shouldWait) {
        return;
    }

    if (success) {
        Sleep(1500);
        return;
    }

    std::wcout << L"Press Enter to close...";
    std::wcin.get();
}

void PrintHelp() {
    std::wcout
        << L"Resolution Switcher\n\n"
        << L"Toggle the primary display between a selected lower resolution\n"
        << L"and 3840x2160. Without a resolution option, the lower resolution\n"
        << L"is 1720x1080.\n\n"
        << L"Resolution options:\n"
        << L"  -1050      Toggle between 1680x1050 and 3840x2160.\n"
        << L"  -1080      Toggle between 1920x1080 and 3840x2160.\n"
        << L"  -1440      Toggle between 2560x1440 and 3840x2160.\n\n"
        << L"Scaling:\n"
        << L"  150%       2560x1440 and 3840x2160.\n"
        << L"  125%       All lower-resolution modes.\n\n"
        << L"Options:\n"
        << L"  --dry-run  Show the selected mode without changing the display.\n"
        << L"  --help     Show this help text.\n";
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    const bool openedDirectly = argc == 1;
    bool dryRun = false;
    Resolution lowerResolution = kDefaultLowerResolution;
    bool hasResolutionOption = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--dry-run") {
            dryRun = true;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintHelp();
            return 0;
        } else if (
            argument == L"-1050" ||
            argument == L"-1080" ||
            argument == L"-1440") {
            if (hasResolutionOption) {
                std::wcerr << L"Error: specify only one resolution option.\n\n";
                PrintHelp();
                return 2;
            }

            hasResolutionOption = true;
            if (argument == L"-1050") {
                lowerResolution = k1050Resolution;
            } else if (argument == L"-1080") {
                lowerResolution = k1080Resolution;
            } else {
                lowerResolution = k1440Resolution;
            }
        } else {
            std::wcerr << L"Unknown option: " << argument << L"\n\n";
            PrintHelp();
            return 2;
        }
    }

    const auto display = FindPrimaryDisplay();
    if (!display) {
        std::wcerr << L"Error: no active primary display was found.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const auto current = ReadCurrentMode(display->DeviceName);
    if (!current) {
        std::wcerr << L"Error: Windows could not read the current display mode.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    if (current->dmDisplayFrequency <= 1 || current->dmBitsPerPel == 0) {
        std::wcerr
            << L"Error: Windows did not report an explicit current refresh rate "
               L"and desktop color depth; switching was cancelled.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const Resolution targetResolution = HasResolution(*current, kUltraHdResolution)
        ? lowerResolution
        : kUltraHdResolution;

    const auto selectedMode = FindPreservingTargetMode(
        display->DeviceName,
        *current,
        targetResolution);

    PrintMode(L"Current: ", *current);

    if (!selectedMode) {
        std::wcerr
            << L"Error: " << targetResolution.width << L'x'
            << targetResolution.height << L" is not available at the current "
            << current->dmDisplayFrequency << L" Hz and "
            << current->dmBitsPerPel
            << L"-bit desktop color depth; switching was cancelled.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }


    NvidiaColorState originalColor;
    std::wstring colorError;
    if (!NvidiaColorControl::ReadPrimary(originalColor, colorError)) {
        std::wcerr
            << L"Error: current NVIDIA color settings could not be read; "
               L"switching was cancelled: "
            << colorError << L'\n';
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }
    if (!NvidiaColorControl::IsSupportedOnPrimary(originalColor, colorError)) {
        std::wcerr
            << L"Error: the NVIDIA driver did not confirm the current color "
               L"settings as supported; switching was cancelled: "
            << colorError << L'\n';
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const DWORD targetDpiPercent = TargetDpiPercent(targetResolution);
    const auto displaySource = FindDisplaySource(display->DeviceName);
    if (!displaySource) {
        std::wcerr
            << L"Error: Windows could not identify the primary display source.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const auto currentDpiScale = ReadDpiScale(*displaySource);
    if (!currentDpiScale) {
        std::wcerr
            << L"Error: Windows could not read the primary display scaling range.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const auto previewDpiRelative = RelativeDpiScaleForPercent(
        *currentDpiScale,
        targetDpiPercent);
    if (!previewDpiRelative) {
        std::wcerr
            << L"Error: " << targetDpiPercent
            << L"% scaling is not supported for the primary display.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const auto currentDpiPercent = DpiPercentForRelativeScale(
        *currentDpiScale,
        currentDpiScale->currentScaleRel);

    PrintMode(dryRun ? L"Would switch to: " : L"Switching to: ", *selectedMode);
    std::wcout
        << L"NVIDIA color settings to preserve: "
        << NvidiaColorControl::Describe(originalColor) << L'\n';
    if (currentDpiPercent) {
        std::wcout << L"DPI scaling: " << *currentDpiPercent << L"% -> ";
    } else {
        std::wcout << L"DPI scaling: ";
    }
    std::wcout << targetDpiPercent << L"%\n";

    if (dryRun) {
        return 0;
    }

    // Use the complete mode returned by the driver. Rebuilding it from the
    // current mode can retain scaling-specific fields that reject the target.
    DEVMODEW target = *selectedMode;

    const LONG testResult = ChangeDisplaySettingsExW(
        display->DeviceName,
        &target,
        nullptr,
        CDS_TEST,
        nullptr);

    if (testResult != DISP_CHANGE_SUCCESSFUL) {
        std::wcerr << L"Error: " << DisplayChangeError(testResult) << L'\n';
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const LONG applyResult = ChangeDisplaySettingsExW(
        display->DeviceName,
        &target,
        nullptr,
        CDS_UPDATEREGISTRY,
        nullptr);

    if (applyResult != DISP_CHANGE_SUCCESSFUL) {
        std::wcerr << L"Error: " << DisplayChangeError(applyResult) << L'\n';
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    NvidiaColorState targetColor;
    bool colorPreserved = NvidiaColorControl::ReadPrimary(
                              targetColor,
                              colorError,
                              20) &&
                          NvidiaColorControl::Equivalent(
                              targetColor,
                              originalColor);

    if (!colorPreserved) {
        if (NvidiaColorControl::IsSupportedOnPrimary(
                originalColor,
                colorError,
                20) &&
            NvidiaColorControl::ApplyToPrimary(
                originalColor,
                colorError,
                20) &&
            NvidiaColorControl::ReadPrimary(
                targetColor,
                colorError,
                20) &&
            NvidiaColorControl::Equivalent(targetColor, originalColor)) {
            colorPreserved = true;
        }
    }

    const auto appliedMode = ReadCurrentMode(display->DeviceName);
    const bool modePreserved =
        appliedMode &&
        HasResolution(*appliedMode, targetResolution) &&
        appliedMode->dmDisplayFrequency == current->dmDisplayFrequency &&
        appliedMode->dmBitsPerPel == current->dmBitsPerPel;

    if (!colorPreserved || !modePreserved) {
        std::wstring restoreError;
        const bool restored = RestoreOriginalDisplayState(
            display->DeviceName,
            *current,
            originalColor,
            *displaySource,
            currentDpiScale->currentScaleRel,
            restoreError);

        if (!modePreserved) {
            std::wcerr
                << L"Error: the driver did not preserve the current refresh rate "
                   L"and desktop color depth; switching was rolled back.\n";
        } else {
            std::wcerr
                << L"Error: the target mode cannot preserve the current NVIDIA "
                   L"color settings; switching was rolled back: "
                << colorError << L'\n';
        }
        if (!restored) {
            std::wcerr << L"Error: " << restoreError << L".\n";
        }
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    // The recommended DPI baseline can change with the active resolution, so
    // recalculate the relative scale after the display mode is applied.
    const auto appliedDpiScale = ReadDpiScale(*displaySource);
    const auto targetDpiRelative = appliedDpiScale
        ? RelativeDpiScaleForPercent(*appliedDpiScale, targetDpiPercent)
        : std::nullopt;
    if (!targetDpiRelative || !SetDpiScale(*displaySource, *targetDpiRelative)) {
        std::wstring restoreError;
        const bool restored = RestoreOriginalDisplayState(
            display->DeviceName,
            *current,
            originalColor,
            *displaySource,
            currentDpiScale->currentScaleRel,
            restoreError);
        std::wcerr
            << L"Error: Windows could not set the display scaling to "
            << targetDpiPercent << L"%.\n";
        if (!restored) {
            std::wcerr << L"Error: " << restoreError << L".\n";
        }
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const auto finalMode = ReadCurrentMode(display->DeviceName);
    NvidiaColorState finalColor;
    const bool finalStatePreserved =
        finalMode &&
        HasResolution(*finalMode, targetResolution) &&
        finalMode->dmDisplayFrequency == current->dmDisplayFrequency &&
        finalMode->dmBitsPerPel == current->dmBitsPerPel &&
        NvidiaColorControl::ReadPrimary(finalColor, colorError, 20) &&
        NvidiaColorControl::Equivalent(finalColor, originalColor);
    if (!finalStatePreserved) {
        std::wstring restoreError;
        const bool restored = RestoreOriginalDisplayState(
            display->DeviceName,
            *current,
            originalColor,
            *displaySource,
            currentDpiScale->currentScaleRel,
            restoreError);
        std::wcerr
            << L"Error: the final display state did not preserve the original "
               L"refresh rate and NVIDIA color settings; switching was rolled back.\n";
        if (!restored) {
            std::wcerr << L"Error: " << restoreError << L".\n";
        }
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    if (!NotifySettingsChange()) {
        std::wcerr
            << L"Warning: Windows did not acknowledge the scaling change; "
               L"sign out or restart may be required.\n";
    }

    std::wcout << L"Done.\n";
    WaitWhenOpenedDirectly(openedDirectly, true);
    return 0;
}
