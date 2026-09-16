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

struct DisplayControl {
    std::wstring deviceName;
    DisplaySource source;
};

struct SuppliedDisplayConfig {
    DISPLAYCONFIG_PATH_INFO path;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
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

bool SameDisplaySource(
    const DisplaySource& left,
    const DisplaySource& right) {
    return left.adapterId.HighPart == right.adapterId.HighPart &&
           left.adapterId.LowPart == right.adapterId.LowPart &&
           left.sourceId == right.sourceId;
}

bool SameAdapter(const LUID& left, const LUID& right) {
    return left.HighPart == right.HighPart &&
           left.LowPart == right.LowPart;
}

bool RefreshRateMatches(
    const DISPLAYCONFIG_RATIONAL& refreshRate,
    const DWORD expectedHz) {
    if (refreshRate.Denominator == 0) {
        return false;
    }

    const auto expected =
        static_cast<unsigned long long>(expectedHz) *
        refreshRate.Denominator;
    const auto reported =
        static_cast<unsigned long long>(refreshRate.Numerator);
    const auto difference = expected > reported
        ? expected - reported
        : reported - expected;
    return difference <= refreshRate.Denominator / 2;
}

std::vector<std::wstring> FindAttachedMonitorPaths(
    const wchar_t* deviceName) {
    std::vector<std::wstring> paths;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        if (!EnumDisplayDevicesW(
                deviceName,
                index,
                &monitor,
                EDD_GET_DEVICE_INTERFACE_NAME)) {
            break;
        }

        const DWORD requiredFlags =
            DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
        if ((monitor.StateFlags & requiredFlags) == requiredFlags &&
            monitor.DeviceID[0] != L'\0') {
            paths.emplace_back(monitor.DeviceID);
        }
    }
    return paths;
}

bool ContainsDevicePath(
    const std::vector<std::wstring>& paths,
    const wchar_t* candidate) {
    for (const auto& path : paths) {
        if (_wcsicmp(path.c_str(), candidate) == 0) {
            return true;
        }
    }
    return false;
}

std::optional<DisplayControl> FindDisplayControl(
    const DISPLAY_DEVICEW& primaryDisplay,
    const DEVMODEW& currentMode) {
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

    const auto monitorPaths = FindAttachedMonitorPaths(
        primaryDisplay.DeviceName);
    std::optional<DisplayControl> selected;
    bool selectedRefreshMatches = false;
    bool selectedNameMatches = false;

    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = paths[index].targetInfo.adapterId;
        targetName.header.id = paths[index].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS) {
            continue;
        }

        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = paths[index].sourceInfo.adapterId;
        sourceName.header.id = paths[index].sourceInfo.id;

        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
            continue;
        }

        const bool monitorMatches = ContainsDevicePath(
            monitorPaths,
            targetName.monitorDevicePath);
        const bool nameMatches =
            _wcsicmp(
                sourceName.viewGdiDeviceName,
                primaryDisplay.DeviceName) == 0;
        if (!monitorMatches && !nameMatches) {
            continue;
        }

        const DisplayControl candidate{
            sourceName.viewGdiDeviceName,
            DisplaySource{
                paths[index].sourceInfo.adapterId,
                paths[index].sourceInfo.id}};
        const bool refreshMatches = RefreshRateMatches(
            paths[index].targetInfo.refreshRate,
            currentMode.dmDisplayFrequency);

        if (!selected) {
            selected = candidate;
            selectedRefreshMatches = refreshMatches;
            selectedNameMatches = nameMatches;
            continue;
        }

        if (SameDisplaySource(selected->source, candidate.source)) {
            selectedRefreshMatches =
                selectedRefreshMatches || refreshMatches;
            selectedNameMatches = selectedNameMatches || nameMatches;
            continue;
        }

        const bool candidatePreferred =
            (refreshMatches && !selectedRefreshMatches) ||
            (refreshMatches == selectedRefreshMatches &&
             nameMatches && !selectedNameMatches);
        if (candidatePreferred) {
            selected = candidate;
            selectedRefreshMatches = refreshMatches;
            selectedNameMatches = nameMatches;
        }
    }

    return selected;
}

std::optional<DisplaySource> FindDatabaseDisplaySource(
    const wchar_t* deviceName) {
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ALL_PATHS,
            &pathCount,
            &modeCount) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            QDC_ALL_PATHS,
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

        if (_wcsicmp(sourceName.viewGdiDeviceName, deviceName) == 0) {
            return DisplaySource{
                paths[index].sourceInfo.adapterId,
                paths[index].sourceInfo.id};
        }
    }

    return std::nullopt;
}

std::optional<SuppliedDisplayConfig>
BuildForcedNvidiaDisplayPortConfiguration(
    const DISPLAY_DEVICEW& primaryDisplay,
    const DEVMODEW& currentMode,
    const Resolution sourceResolution) {
    if (std::wstring_view{primaryDisplay.DeviceString}.find(L"NVIDIA") ==
        std::wstring_view::npos) {
        return std::nullopt;
    }

    const auto primarySource = FindDatabaseDisplaySource(
        primaryDisplay.DeviceName);
    if (!primarySource) {
        return std::nullopt;
    }

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(
            QDC_ALL_PATHS,
            &pathCount,
            &modeCount) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            QDC_ALL_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return std::nullopt;
    }

    const auto monitorPaths = FindAttachedMonitorPaths(
        primaryDisplay.DeviceName);
    std::wstring displayPortMonitorPath;
    std::optional<DISPLAYCONFIG_TARGET_MODE> activeTargetMode;
    for (UINT32 index = 0; index < pathCount; ++index) {
        const auto& path = paths[index];
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            path.targetInfo.outputTechnology !=
                DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL ||
            !RefreshRateMatches(
                path.targetInfo.refreshRate,
                currentMode.dmDisplayFrequency) ||
            path.targetInfo.modeInfoIdx ==
                DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS ||
            !ContainsDevicePath(
                monitorPaths,
                targetName.monitorDevicePath)) {
            continue;
        }

        displayPortMonitorPath = targetName.monitorDevicePath;
        activeTargetMode = modes[path.targetInfo.modeInfoIdx].targetMode;
        break;
    }

    if (displayPortMonitorPath.empty() || !activeTargetMode) {
        return std::nullopt;
    }

    for (UINT32 index = 0; index < pathCount; ++index) {
        auto path = paths[index];
        if (!SameAdapter(
                path.targetInfo.adapterId,
                primarySource->adapterId) ||
            path.targetInfo.outputTechnology !=
                DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS ||
            _wcsicmp(
                targetName.monitorDevicePath,
                displayPortMonitorPath.c_str()) != 0) {
            continue;
        }

        DISPLAYCONFIG_MODE_INFO sourceMode{};
        sourceMode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        sourceMode.id = path.sourceInfo.id;
        sourceMode.adapterId = path.sourceInfo.adapterId;
        sourceMode.sourceMode.width = sourceResolution.width;
        sourceMode.sourceMode.height = sourceResolution.height;
        sourceMode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
        sourceMode.sourceMode.position = {0, 0};

        DISPLAYCONFIG_MODE_INFO targetMode{};
        targetMode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        targetMode.id = path.targetInfo.id;
        targetMode.adapterId = path.targetInfo.adapterId;
        targetMode.targetMode = *activeTargetMode;

        path.sourceInfo.modeInfoIdx = 0;
        path.targetInfo.modeInfoIdx = 1;
        path.targetInfo.refreshRate =
            activeTargetMode->targetVideoSignalInfo.vSyncFreq;
        path.targetInfo.scanLineOrdering =
            activeTargetMode->targetVideoSignalInfo.scanLineOrdering;
        if (sourceResolution.width !=
                activeTargetMode->targetVideoSignalInfo.activeSize.cx ||
            sourceResolution.height !=
                activeTargetMode->targetVideoSignalInfo.activeSize.cy) {
            path.targetInfo.scaling =
                DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;
        }
        path.flags = DISPLAYCONFIG_PATH_ACTIVE;

        SuppliedDisplayConfig configuration{
            path,
            std::vector<DISPLAYCONFIG_MODE_INFO>{sourceMode, targetMode}};
        auto validatePath = configuration.path;
        auto validateModes = configuration.modes;
        const LONG validateResult = SetDisplayConfig(
            1,
            &validatePath,
            static_cast<UINT32>(validateModes.size()),
            validateModes.data(),
            SDC_VALIDATE |
                SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                SDC_ALLOW_CHANGES);
        if (validateResult == ERROR_SUCCESS) {
            return configuration;
        }
    }

    return std::nullopt;
}

LONG ApplySuppliedDisplayConfig(
    const SuppliedDisplayConfig& configuration) {
    auto path = configuration.path;
    auto modes = configuration.modes;
    LONG result = SetDisplayConfig(
        1,
        &path,
        static_cast<UINT32>(modes.size()),
        modes.data(),
        SDC_APPLY |
            SDC_USE_SUPPLIED_DISPLAY_CONFIG |
            SDC_ALLOW_CHANGES |
            SDC_SAVE_TO_DATABASE);
    if (result == ERROR_INVALID_PARAMETER) {
        path = configuration.path;
        modes = configuration.modes;
        result = SetDisplayConfig(
            1,
            &path,
            static_cast<UINT32>(modes.size()),
            modes.data(),
            SDC_APPLY |
                SDC_USE_SUPPLIED_DISPLAY_CONFIG |
                SDC_ALLOW_CHANGES);
    }
    return result;
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
    if (resolution.width == k1440Resolution.width &&
        resolution.height == k1440Resolution.height) {
        return 100;
    }

    return resolution.height == kUltraHdResolution.height ? 150 : 125;
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

void ShowShortcutError(const bool openedDirectly, const std::wstring& message) {
    if (openedDirectly) {
        return;
    }

    MessageBoxW(
        nullptr,
        message.c_str(),
        L"Res Switcher",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
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
        << L"  150%       3840x2160.\n"
        << L"  100%       2560x1440.\n"
        << L"  125%       Other lower-resolution modes.\n\n"
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

    auto display = FindPrimaryDisplay();
    if (!display) {
        std::wcerr << L"Error: no active primary display was found.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    auto current = ReadCurrentMode(display->DeviceName);
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

    auto displayControl = FindDisplayControl(*display, *current);
    if (!displayControl) {
        const std::wstring error =
            L"Error: Windows could not identify the active display path for "
            L"the primary display.\n\nThe current display mode was left "
            L"unchanged.";
        std::wcerr << error << L'\n';
        ShowShortcutError(openedDirectly, error);
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const Resolution targetResolution = HasResolution(*current, kUltraHdResolution)
        ? lowerResolution
        : kUltraHdResolution;

    const Resolution currentResolution{
        current->dmPelsWidth,
        current->dmPelsHeight};
    bool wouldForceNvidiaDisplayPort = false;
    const auto primaryDatabaseSource = FindDatabaseDisplaySource(
        display->DeviceName);
    const bool mustForceNvidiaDisplayPort =
        primaryDatabaseSource &&
        std::wstring_view{display->DeviceString}.find(L"NVIDIA") !=
            std::wstring_view::npos &&
        !SameAdapter(
            displayControl->source.adapterId,
            primaryDatabaseSource->adapterId);

    if (mustForceNvidiaDisplayPort) {
        const auto currentNvidiaConfiguration =
            BuildForcedNvidiaDisplayPortConfiguration(
                *display,
                *current,
                currentResolution);
        const auto targetNvidiaConfiguration =
            BuildForcedNvidiaDisplayPortConfiguration(
                *display,
                *current,
                targetResolution);
        if (!currentNvidiaConfiguration || !targetNvidiaConfiguration) {
            const std::wstring error =
                L"Error: the RTX display's physical DisplayPort path could "
                L"not be validated; switching was cancelled.\n\nThe current "
                L"display mode was left unchanged.";
            std::wcerr << error << L'\n';
            ShowShortcutError(openedDirectly, error);
            WaitWhenOpenedDirectly(openedDirectly, false);
            return 1;
        }

        if (dryRun) {
            wouldForceNvidiaDisplayPort = true;
        } else {
            std::wcout
                << L"Activating the RTX DisplayPort output and disabling the "
                   L"AMD HDMI clone...\n";
            NvidiaColorState topologyColor;
            std::wstring topologyColorError;
            if (!NvidiaColorControl::ReadPrimary(
                    topologyColor,
                    topologyColorError) ||
                !NvidiaColorControl::IsSupportedOnPrimary(
                    topologyColor,
                    topologyColorError)) {
                const std::wstring error =
                    L"Error: NVIDIA color settings could not be protected "
                    L"before activating the RTX DisplayPort path: " +
                    topologyColorError;
                std::wcerr << error << L'\n';
                ShowShortcutError(openedDirectly, error);
                WaitWhenOpenedDirectly(openedDirectly, false);
                return 1;
            }

            const LONG topologyResult = ApplySuppliedDisplayConfig(
                *currentNvidiaConfiguration);
            if (topologyResult != ERROR_SUCCESS) {
                const std::wstring error =
                    L"Error: Windows could not activate the RTX DisplayPort "
                    L"path (error " + std::to_wstring(topologyResult) +
                    L").\n\nThe current display mode was left unchanged.";
                std::wcerr << error << L'\n';
                ShowShortcutError(openedDirectly, error);
                WaitWhenOpenedDirectly(openedDirectly, false);
                return 1;
            }

            bool normalized = false;
            for (unsigned int attempt = 0; attempt < 30; ++attempt) {
                if (attempt != 0) {
                    Sleep(100);
                }
                auto nextDisplay = FindPrimaryDisplay();
                if (!nextDisplay) {
                    continue;
                }
                auto nextCurrent = ReadCurrentMode(nextDisplay->DeviceName);
                if (!nextCurrent) {
                    continue;
                }
                auto nextControl = FindDisplayControl(
                    *nextDisplay,
                    *nextCurrent);
                const auto nextDatabaseSource = FindDatabaseDisplaySource(
                    nextDisplay->DeviceName);
                if (!nextControl || !nextDatabaseSource ||
                    !SameAdapter(
                        nextControl->source.adapterId,
                        nextDatabaseSource->adapterId) ||
                    !HasResolution(*nextCurrent, currentResolution) ||
                    nextCurrent->dmDisplayFrequency !=
                        current->dmDisplayFrequency ||
                    nextCurrent->dmBitsPerPel != current->dmBitsPerPel) {
                    continue;
                }

                display = nextDisplay;
                current = nextCurrent;
                displayControl = nextControl;
                normalized = true;
                break;
            }

            NvidiaColorState normalizedColor;
            bool colorPreserved =
                NvidiaColorControl::ReadPrimary(
                    normalizedColor,
                    topologyColorError,
                    20) &&
                NvidiaColorControl::Equivalent(
                    normalizedColor,
                    topologyColor);
            if (!colorPreserved &&
                NvidiaColorControl::IsSupportedOnPrimary(
                    topologyColor,
                    topologyColorError,
                    20) &&
                NvidiaColorControl::ApplyToPrimary(
                    topologyColor,
                    topologyColorError,
                    20) &&
                NvidiaColorControl::ReadPrimary(
                    normalizedColor,
                    topologyColorError,
                    20) &&
                NvidiaColorControl::Equivalent(
                    normalizedColor,
                    topologyColor)) {
                colorPreserved = true;
            }

            if (!normalized || !colorPreserved) {
                const std::wstring error = !normalized
                    ? L"Error: Windows did not activate the RTX DisplayPort "
                      L"path as the primary display."
                    : L"Error: the RTX DisplayPort path was activated, but "
                      L"the NVIDIA color settings could not be preserved: " +
                      topologyColorError;
                std::wcerr << error << L'\n';
                ShowShortcutError(openedDirectly, error);
                WaitWhenOpenedDirectly(openedDirectly, false);
                return 1;
            }
        }
    }

    std::optional<DEVMODEW> originalControlMode;
    std::optional<DEVMODEW> selectedMode;
    if (wouldForceNvidiaDisplayPort) {
        originalControlMode = *current;
        DEVMODEW preview = *current;
        preview.dmPelsWidth = targetResolution.width;
        preview.dmPelsHeight = targetResolution.height;
        selectedMode = preview;
    } else {
        originalControlMode = FindPreservingTargetMode(
            displayControl->deviceName.c_str(),
            *current,
            currentResolution);
        selectedMode = FindPreservingTargetMode(
            displayControl->deviceName.c_str(),
            *current,
            targetResolution);
    }
    if (!originalControlMode) {
        const std::wstring error =
            L"Error: the current display mode could not be validated on the "
            L"active display path; switching was cancelled.\n\nThe current "
            L"display mode was left unchanged.";
        std::wcerr << error << L'\n';
        ShowShortcutError(openedDirectly, error);
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    PrintMode(L"Current: ", *current);

    if (!selectedMode) {
        const std::wstring error =
            L"Error: " + std::to_wstring(targetResolution.width) + L'x' +
            std::to_wstring(targetResolution.height) +
            L" is not available at the current " +
            std::to_wstring(current->dmDisplayFrequency) + L" Hz and " +
            std::to_wstring(current->dmBitsPerPel) +
            L"-bit desktop color depth; switching was cancelled.\n\n"
            L"The current display mode was left unchanged.";
        std::wcerr << error << L'\n';
        ShowShortcutError(openedDirectly, error);
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
    const auto displaySource = FindDatabaseDisplaySource(display->DeviceName);
    if (!displaySource) {
        std::wcerr
            << L"Error: Windows could not identify the primary display's "
               L"DPI source.\n";
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

    if (wouldForceNvidiaDisplayPort) {
        std::wcout
            << L"Display path: would disable the AMD HDMI clone and activate "
               L"the RTX DisplayPort output.\n";
    }
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
        displayControl->deviceName.c_str(),
        &target,
        nullptr,
        CDS_TEST,
        nullptr);

    if (testResult != DISP_CHANGE_SUCCESSFUL) {
        const std::wstring error =
            L"Error: " + std::wstring{DisplayChangeError(testResult)} +
            L"\n\nThe current display mode was left unchanged.";
        std::wcerr << error << L'\n';
        ShowShortcutError(openedDirectly, error);
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    const LONG applyResult = ChangeDisplaySettingsExW(
        displayControl->deviceName.c_str(),
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
            displayControl->deviceName.c_str(),
            *originalControlMode,
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
            displayControl->deviceName.c_str(),
            *originalControlMode,
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
            displayControl->deviceName.c_str(),
            *originalControlMode,
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
