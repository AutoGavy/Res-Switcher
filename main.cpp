#include <windows.h>

#include "nv_color_state.h"
#include "power_plan.h"
#include "display_config.h"
#include "switch_safety.h"

#include <iostream>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
constexpr DWORD kPreferredRefreshHz = 240;

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

using SuppliedDisplayConfig = DisplayConfiguration;

struct DpiScaleInfo {
    LONG minScaleRel;
    LONG currentScaleRel;
    LONG maxScaleRel;
};

bool ValidDpiScale(const DpiScaleInfo& scale) {
    const auto count = static_cast<LONG>(std::size(kDpiScaleValues));
    return scale.minScaleRel <= 0 && scale.minScaleRel > -count &&
        scale.maxScaleRel >= 0 && scale.maxScaleRel < count + scale.minScaleRel &&
        scale.currentScaleRel >= scale.minScaleRel &&
        scale.currentScaleRel <= scale.maxScaleRel;
}

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

constexpr bool SameResolution(const Resolution left, const Resolution right) {
    return left.width == right.width && left.height == right.height;
}

constexpr Resolution SelectTargetResolution(
    const Resolution current,
    const Resolution lower,
    const DWORD currentRefreshHz) {
    if (currentRefreshHz != kPreferredRefreshHz) {
        return kUltraHdResolution;
    }
    return SameResolution(current, kUltraHdResolution)
        ? lower
        : SameResolution(current, lower) ? kUltraHdResolution : lower;
}

static_assert(SameResolution(
    SelectTargetResolution(k1080Resolution, k1440Resolution, 144),
    kUltraHdResolution));
static_assert(SameResolution(
    SelectTargetResolution(kUltraHdResolution, k1080Resolution, 60),
    kUltraHdResolution));
static_assert(SameResolution(
    SelectTargetResolution(kUltraHdResolution, k1080Resolution, 240),
    k1080Resolution));
static_assert(SameResolution(
    SelectTargetResolution(k1080Resolution, k1080Resolution, 240),
    kUltraHdResolution));
static_assert(SameResolution(
    SelectTargetResolution(k1080Resolution, k1440Resolution, 240),
    k1440Resolution));

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
    const auto configuration = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
    if (!configuration) {
        return std::nullopt;
    }
    const auto& paths = configuration->paths;

    const auto monitorPaths = FindAttachedMonitorPaths(
        primaryDisplay.DeviceName);
    std::optional<DisplayControl> selected;
    bool selectedRefreshMatches = false;
    bool selectedNameMatches = false;

    for (std::size_t index = 0; index < paths.size(); ++index) {
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
    const auto configuration = ReadDisplayConfiguration(QDC_ALL_PATHS);
    if (!configuration) {
        return std::nullopt;
    }
    const auto& paths = configuration->paths;

    for (std::size_t index = 0; index < paths.size(); ++index) {
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
    const Resolution sourceResolution,
    const DisplayControl& currentControl) {
    if (std::wstring_view{primaryDisplay.DeviceString}.find(L"NVIDIA") ==
        std::wstring_view::npos) {
        return std::nullopt;
    }

    const auto primarySource = FindDatabaseDisplaySource(
        primaryDisplay.DeviceName);
    if (!primarySource) {
        return std::nullopt;
    }

    const auto all = ReadDisplayConfiguration(QDC_ALL_PATHS);
    if (!all) {
        return std::nullopt;
    }
    const auto& paths = all->paths;
    const auto& modes = all->modes;

    const auto monitorPaths = FindAttachedMonitorPaths(
        primaryDisplay.DeviceName);
    std::wstring displayPortMonitorPath;
    std::optional<DISPLAYCONFIG_TARGET_MODE> activeTargetMode;
    DISPLAYCONFIG_ROTATION activeRotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const auto& path = paths[index];
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            path.targetInfo.outputTechnology !=
                DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL ||
            !RefreshRateMatches(
                path.targetInfo.refreshRate,
                currentMode.dmDisplayFrequency) ||
            !ValidDisplayModeIndex(*all, path.targetInfo.modeInfoIdx,
                DISPLAYCONFIG_MODE_INFO_TYPE_TARGET,
                path.targetInfo.adapterId, path.targetInfo.id)) {
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
        activeRotation = path.targetInfo.rotation;
        break;
    }

    if (displayPortMonitorPath.empty() || !activeTargetMode) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < paths.size(); ++index) {
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
        path.targetInfo.rotation = activeRotation;
        if (sourceResolution.width !=
                activeTargetMode->targetVideoSignalInfo.activeSize.cx ||
            sourceResolution.height !=
                activeTargetMode->targetVideoSignalInfo.activeSize.cy) {
            path.targetInfo.scaling =
                DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;
        }
        path.flags = DISPLAYCONFIG_PATH_ACTIVE;

        auto configuration = ReplacePrimaryDisplayPaths(*all,
            currentControl.source.adapterId, currentControl.source.sourceId,
            path, sourceMode, targetMode);
        if (!configuration) {
            continue;
        }
        auto validatePaths = configuration->paths;
        auto validateModes = configuration->modes;
        const LONG validateResult = SetDisplayConfig(
            static_cast<UINT32>(validatePaths.size()),
            validatePaths.data(),
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
    LONG result = ApplyDisplayConfiguration(configuration, true, true);
    if (result == ERROR_INVALID_PARAMETER) {
        result = ApplyDisplayConfiguration(configuration, false, true);
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

    const DpiScaleInfo scale{
        request.minScaleRel,
        request.currentScaleRel,
        request.maxScaleRel};
    // Validate before negating/indexing driver-provided signed values.
    if (!ValidDpiScale(scale)) {
        return std::nullopt;
    }
    return scale;
}

std::optional<LONG> RelativeDpiScaleForPercent(
    const DpiScaleInfo& scale,
    const DWORD targetPercent) {
    if (!ValidDpiScale(scale)) { return std::nullopt; }
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
    const auto relativeScale = static_cast<long long>(targetIndex) + scale.minScaleRel;
    if (relativeScale < scale.minScaleRel ||
        relativeScale > scale.maxScaleRel) {
        return std::nullopt;
    }

    return static_cast<LONG>(relativeScale);
}

std::optional<DWORD> DpiPercentForRelativeScale(
    const DpiScaleInfo& scale,
    const LONG relativeScale) {
    if (!ValidDpiScale(scale) || relativeScale < scale.minScaleRel ||
        relativeScale > scale.maxScaleRel) { return std::nullopt; }
    const auto index = -static_cast<long long>(scale.minScaleRel) + relativeScale;
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
    // No pointer payload, so this can be queued without waiting on hung apps.
    return SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0) != 0;
}

constexpr DWORD TargetDpiPercent(const Resolution resolution) {
    if (resolution.width == kDefaultLowerResolution.width &&
        resolution.height == kDefaultLowerResolution.height) {
        return 100;
    }
    if (resolution.width == k1080Resolution.width &&
        resolution.height == k1080Resolution.height) {
        return 100;
    }

    return resolution.height == kUltraHdResolution.height ? 150 : 125;
}

static_assert(TargetDpiPercent(kDefaultLowerResolution) == 100);
static_assert(TargetDpiPercent(k1080Resolution) == 100);
static_assert(TargetDpiPercent(k1440Resolution) == 125);
static_assert(TargetDpiPercent(kUltraHdResolution) == 150);
static_assert(TargetDpiPercent(k1050Resolution) == 125);

std::optional<DEVMODEW> FindPreservingTargetMode(
    const wchar_t* deviceName,
    const DEVMODEW& current,
    const Resolution targetResolution,
    const DWORD targetRefreshHz) {
    std::optional<DEVMODEW> enumeratedMode;
    for (DWORD index = 0;; ++index) {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);

        if (!EnumDisplaySettingsExW(deviceName, index, &candidate, 0)) {
            break;
        }

        if (!HasResolution(candidate, targetResolution) ||
            candidate.dmDisplayFrequency != targetRefreshHz ||
            candidate.dmBitsPerPel != current.dmBitsPerPel ||
            candidate.dmDisplayOrientation != current.dmDisplayOrientation ||
            (candidate.dmDisplayFlags & DM_INTERLACED) !=
                (current.dmDisplayFlags & DM_INTERLACED)) {
            continue;
        }

        // Some NVIDIA/Windows configurations reject CDS_TEST even for the
        // active mode. Prefer a tested mode, but retain an exact enumerated
        // match for real application followed by validation and rollback.
        if (!enumeratedMode) {
            enumeratedMode = candidate;
        }
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

    return enumeratedMode;
}

bool SameCoreMode(const DEVMODEW& actual, const DEVMODEW& expected) {
    return actual.dmPelsWidth == expected.dmPelsWidth &&
        actual.dmPelsHeight == expected.dmPelsHeight &&
        actual.dmDisplayFrequency == expected.dmDisplayFrequency &&
        actual.dmBitsPerPel == expected.dmBitsPerPel &&
        actual.dmDisplayOrientation == expected.dmDisplayOrientation &&
        (actual.dmDisplayFlags & DM_INTERLACED) == (expected.dmDisplayFlags & DM_INTERLACED);
}

bool SetDpiPercentAndVerify(const DisplaySource& source, const DWORD percent) {
    auto scale = ReadDpiScale(source);
    const auto relative = scale ? RelativeDpiScaleForPercent(*scale, percent) : std::nullopt;
    if (!relative) { return false; }
    if (scale->currentScaleRel != *relative && !SetDpiScale(source, *relative)) { return false; }
    for (unsigned int attempt = 0; attempt < 10; ++attempt) {
        if (attempt != 0) { Sleep(50); }
        scale = ReadDpiScale(source);
        if (scale && DpiPercentForRelativeScale(*scale, scale->currentScaleRel) == percent) {
            return true;
        }
    }
    return false;
}

struct DisplaySnapshot {
    DisplayConfiguration active;
    DisplayConfiguration saved;
    DisplayConfiguration protectedViews;
    DEVMODEW primaryMode{};
    NvidiaColorState color;
    DisplaySource dpiSource{};
    DWORD dpiPercent = 0;
};

std::optional<DisplaySnapshot> CaptureDisplaySnapshot(
    const DisplayControl& control, const DEVMODEW& mode,
    const NvidiaColorState& color) {
    const auto active = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
    const auto saved = ReadDisplayConfiguration(QDC_DATABASE_CURRENT);
    const auto dpi = ReadDpiScale(control.source);
    const auto percent = dpi ? DpiPercentForRelativeScale(*dpi, dpi->currentScaleRel) : std::nullopt;
    const auto primary = FindPrimaryDisplay();
    const auto observedMode = primary ? ReadCurrentMode(primary->DeviceName) : std::nullopt;
    const auto observedControl = primary && observedMode
        ? FindDisplayControl(*primary, *observedMode) : std::nullopt;
    if (!active || !saved || !percent || !RestorableDisplayConfiguration(*active) ||
        !RestorableDisplayConfiguration(*saved) || !observedMode ||
        !SameCoreMode(*observedMode, mode) || !observedControl ||
        !SameDisplaySource(observedControl->source, control.source)) { return std::nullopt; }
    auto protectedViews = *active;
    std::erase_if(protectedViews.paths, [&](const auto& path) {
        return SameAdapter(path.sourceInfo.adapterId, control.source.adapterId) &&
            path.sourceInfo.id == control.source.sourceId;
    });
    return DisplaySnapshot{*active, *saved, std::move(protectedViews), mode, color, control.source, *percent};
}

bool RestoreDisplaySnapshot(const DisplaySnapshot& snapshot, std::wstring& error) {
    error.clear();
    // An API can reject a change without touching anything. In that case do
    // not force an unnecessary full display reset (and another black flash).
    const auto before = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
    const auto beforeSaved = ReadDisplayConfiguration(QDC_DATABASE_CURRENT);
    const auto beforePrimary = FindPrimaryDisplay();
    const auto beforeMode = beforePrimary ? ReadCurrentMode(beforePrimary->DeviceName) : std::nullopt;
    const auto beforeDpi = ReadDpiScale(snapshot.dpiSource);
    if (before && beforeSaved && beforeMode && beforeDpi &&
        SameCoreMode(*beforeMode, snapshot.primaryMode) &&
        EquivalentDisplayConfigurations(snapshot.active, *before) &&
        EquivalentDisplayConfigurations(snapshot.saved, *beforeSaved) &&
        DpiPercentForRelativeScale(*beforeDpi, beforeDpi->currentScaleRel) == snapshot.dpiPercent) {
        NvidiaColorState color;
        std::wstring colorError;
        if (NvidiaColorControl::ReadPrimary(color, colorError, 20) &&
            NvidiaColorControl::Equivalent(color, snapshot.color)) { return true; }
    }
    const auto append = [&](const std::wstring& detail) {
        if (!error.empty()) { error += L"; "; }
        error += detail;
    };
    // Restore the persisted configuration separately if it differed from the
    // active (temporary) one at launch. Otherwise one apply is sufficient.
    if (!EquivalentDisplayConfigurations(snapshot.saved, snapshot.active) &&
        ApplyDisplayConfiguration(snapshot.saved, true, false) != ERROR_SUCCESS) {
        append(L"the saved display configuration could not be restored");
    }
    const bool sameSaved = EquivalentDisplayConfigurations(snapshot.saved, snapshot.active);
    const LONG result = ApplyDisplayConfiguration(snapshot.active, sameSaved, false);
    if (result != ERROR_SUCCESS) {
        append(L"the original display topology could not be restored (Windows error " +
            std::to_wstring(result) + L")");
    }
    const auto actual = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
    const auto primary = FindPrimaryDisplay();
    const auto mode = primary ? ReadCurrentMode(primary->DeviceName) : std::nullopt;
    const bool topologyRestored = actual &&
        EquivalentDisplayConfigurations(snapshot.active, *actual) && mode &&
        SameCoreMode(*mode, snapshot.primaryMode);
    if (!topologyRestored) {
        append(L"the original topology/mode could not be verified");
    } else {
        NvidiaColorState color;
        std::wstring colorError;
        const bool already = NvidiaColorControl::ReadPrimary(color, colorError, 20) &&
            NvidiaColorControl::Equivalent(color, snapshot.color);
        if (!already && (!NvidiaColorControl::ApplyToPrimary(snapshot.color, colorError, 20) ||
                !NvidiaColorControl::ReadPrimary(color, colorError, 20) ||
                !NvidiaColorControl::Equivalent(color, snapshot.color))) {
            append(L"the original NVIDIA colors could not be restored: " + colorError);
        }
    }
    // Attempt DPI recovery even if the color driver failed. Use an absolute
    // percentage because the recommended baseline may have changed.
    if (!SetDpiPercentAndVerify(snapshot.dpiSource, snapshot.dpiPercent)) {
        append(L"the original DPI scaling could not be restored");
    }
    const auto saved = ReadDisplayConfiguration(QDC_DATABASE_CURRENT);
    if (!saved || !EquivalentDisplayConfigurations(snapshot.saved, *saved)) {
        append(L"the persisted display configuration could not be verified");
    }
    NotifySettingsChange();
    return error.empty();
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

void ShowBatteryResult(const PowerPlanKind kind) {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const bool hasConsole = GetConsoleScreenBufferInfo(output, &info) != FALSE;

    std::wcout << L"\n==================================================\n"
        << L"  Current power plan: " << std::flush;
    const WORD foregroundMask = FOREGROUND_RED | FOREGROUND_GREEN |
        FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    const WORD green = static_cast<WORD>((info.wAttributes & ~foregroundMask) |
        FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    const bool highlighted = hasConsole && SetConsoleTextAttribute(output, green) != FALSE;
    std::wcout << PowerPlanLabel(kind) << std::flush;
    if (highlighted) {
        SetConsoleTextAttribute(output, info.wAttributes);
    }
    std::wcout << L"\n==================================================\n" << std::flush;
    Sleep(1000);
}

void ShowShortcutError(const bool suppressDialog, const std::wstring& message) {
    if (suppressDialog) {
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
        << L"is 1720x1080. At any refresh rate other than 240 Hz,\n"
        << L"switch to 3840x2160 at 240 Hz regardless of resolution option.\n\n"
        << L"Resolution options:\n"
        << L"  -1050      Toggle between 1680x1050 and 3840x2160.\n"
        << L"  -1080      Toggle between 1920x1080 and 3840x2160.\n"
        << L"  -1440      Toggle between 2560x1440 and 3840x2160.\n\n"
        << L"Scaling:\n"
        << L"  150%       3840x2160.\n"
        << L"  100%       1720x1080 and 1920x1080.\n"
        << L"  125%       1680x1050 and 2560x1440.\n\n"
        << L"Power plans (selected from the actual target resolution):\n"
        << L"  Balanced              3840x2160.\n"
        << L"  Ultimate Performance  Lower resolutions.\n\n"
        << L"Options:\n"
        << L"  --restore-4k  Ensure 3840x2160 at 240 Hz, without prompts.\n"
        << L"                Alias: -restore4K. Always use Balanced.\n"
        << L"                Already at this mode: leave the display unchanged\n"
        << L"                and ensure the Balanced power plan is active.\n"
        << L"  -battery      Restore 4K at 240 Hz, then toggle Balanced and\n"
        << L"                Ultimate Performance, without prompts. Other active\n"
        << L"                plans switch to Balanced. Already at 4K at 240 Hz:\n"
        << L"                leave the display unchanged; only toggle the plan.\n"
        << L"                Show the current plan in green for 1 second on success.\n"
        << L"  --dry-run  Preview only; do not change the display or power plan.\n"
        << L"  --help     Show this help text.\n";
}

} // namespace

int RunProgram(const int argc, wchar_t* argv[]) {
    const bool openedDirectly = argc == 1;
    bool dryRun = false;
    bool restore4k = false;
    bool batteryMode = false;
    Resolution lowerResolution = kDefaultLowerResolution;
    bool hasResolutionOption = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--dry-run") {
            dryRun = true;
        } else if (argument == L"--restore-4k" || argument == L"-restore4K") {
            if (restore4k) {
                std::wcerr << L"Error: specify --restore-4k only once.\n";
                return 2;
            }
            restore4k = true;
        } else if (argument == L"-battery") {
            if (batteryMode) {
                std::wcerr << L"Error: specify -battery only once.\n";
                return 2;
            }
            batteryMode = true;
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

    if (restore4k && batteryMode) {
        std::wcerr << L"Error: -battery cannot be combined with --restore-4k.\n";
        return 2;
    }
    const bool ensure4k = restore4k || batteryMode;
    if (ensure4k && hasResolutionOption) {
        std::wcerr
            << L"Error: --restore-4k and -battery cannot be combined with a resolution option.\n";
        return 2;
    }
    const bool suppressErrorDialog = openedDirectly || ensure4k || dryRun;

    SwitchLock switchLock;
    if (!switchLock.Acquired()) {
        std::wcerr << L"Error: another display switch is in progress, or the switch lock is unavailable.\n";
        return 1;
    }
    std::optional<DisplaySnapshot> snapshot;
    std::wstring recoveryMessage;
    RollbackAction rollback([&] {
        std::wstring recoveryError;
        if (snapshot && RestoreDisplaySnapshot(*snapshot, recoveryError)) {
            recoveryMessage = L"Original display topology, mode, colors and scaling verified (unchanged or restored).";
        } else {
            recoveryMessage = L"Warning: display rollback was not fully confirmed: " + recoveryError;
        }
        std::wcerr << recoveryMessage << L'\n';
    });
    const auto fail = [&](const std::wstring& error) {
        // Restore before opening a dialog or waiting for Enter on a bad mode.
        std::wcerr << error << L'\n';
        rollback.Run();
        switchLock.Release();
        ShowShortcutError(suppressErrorDialog,
            recoveryMessage.empty() ? error : error + L"\n\n" + recoveryMessage);
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    };
    const auto prepareRollback = [&](const DisplayControl& control,
        const DEVMODEW& mode, const NvidiaColorState& color) {
        if (!snapshot) { snapshot = CaptureDisplaySnapshot(control, mode, color); }
        return snapshot.has_value();
    };

    auto display = FindPrimaryDisplay();
    if (!display) {
        return fail(L"Error: no active primary display was found.");
    }

    auto current = ReadCurrentMode(display->DeviceName);
    if (!current) {
        return fail(L"Error: Windows could not read the current display mode.");
    }

    if (current->dmDisplayFrequency <= 1 || current->dmBitsPerPel == 0) {
        return fail(L"Error: Windows did not report an explicit refresh rate and color depth; switching was cancelled.");
    }

    const Resolution currentResolution{current->dmPelsWidth, current->dmPelsHeight};
    const Resolution targetResolution = ensure4k ? kUltraHdResolution : SelectTargetResolution(
        currentResolution, lowerResolution, current->dmDisplayFrequency);
    const DWORD targetRefreshHz = kPreferredRefreshHz;

    std::wstring powerError;
    const auto targetPowerPlan = batteryMode
        ? FindToggledPowerPlan(WindowsPowerPlans(), powerError)
        : FindPowerPlan(WindowsPowerPlans(),
            restore4k ? PowerPlanKind::Balanced :
                PowerPlanForResolution(targetResolution.width, targetResolution.height), powerError);
    if (!targetPowerPlan) {
        const std::wstring error = L"Error: " + powerError +
            L"\nThe display and power plan were left unchanged.";
        return fail(error);
    }

    if (ensure4k && HasResolution(*current, kUltraHdResolution) &&
        current->dmDisplayFrequency == kPreferredRefreshHz) {
        PrintMode(L"Current: ", *current);
        std::wcout << L"Already at 3840x2160 @ 240 Hz; no display changes needed.\n";
        if (!dryRun && !ActivatePowerPlan(WindowsPowerPlans(), *targetPowerPlan, powerError)) {
            std::wcerr << L"Error: " << powerError << L'\n';
            return 1;
        }
        if (batteryMode && !dryRun) {
            switchLock.Release();
            ShowBatteryResult(targetPowerPlan->kind);
        } else {
            std::wcout << (dryRun ? L"Would use power plan: " : L"Power plan: ")
                << DescribePowerPlan(*targetPowerPlan) << L'\n';
        }
        return 0;
    }

    auto displayControl = FindDisplayControl(*display, *current);
    if (!displayControl) {
        const std::wstring error =
            L"Error: Windows could not identify the active display path for "
            L"the primary display.\n\nThe current display mode was left "
            L"unchanged.";
        return fail(error);
    }

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
                currentResolution,
                *displayControl);
        const auto targetNvidiaConfiguration =
            BuildForcedNvidiaDisplayPortConfiguration(
                *display,
                *current,
                targetResolution,
                *displayControl);
        if (!currentNvidiaConfiguration || !targetNvidiaConfiguration) {
            const std::wstring error =
                L"Error: the RTX display's physical DisplayPort path could "
                L"not be validated; switching was cancelled.\n\nThe current "
                L"display mode was left unchanged.";
            return fail(error);
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
                return fail(error);
            }

            if (!prepareRollback(*displayControl, *current, topologyColor)) {
                return fail(L"Error: the original display state could not be saved; no changes made.");
            }
            const auto& primaryTarget = currentNvidiaConfiguration->paths.front().targetInfo;
            std::erase_if(snapshot->protectedViews.paths, [&](const auto& path) {
                return SameAdapter(path.targetInfo.adapterId, primaryTarget.adapterId) &&
                    path.targetInfo.id == primaryTarget.id;
            });
            rollback.Arm();
            const LONG topologyResult = ApplySuppliedDisplayConfig(
                *currentNvidiaConfiguration);
            if (topologyResult != ERROR_SUCCESS) {
                const std::wstring error =
                    L"Error: Windows could not activate the RTX DisplayPort "
                    L"path (error " + std::to_wstring(topologyResult) +
                    L").";
                return fail(error);
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
                    !SameCoreMode(*nextCurrent, *current)) {
                    continue;
                }

                display = nextDisplay;
                current = nextCurrent;
                displayControl = nextControl;
                normalized = true;
                break;
            }

            if (!normalized) {
                return fail(L"Error: Windows did not activate the RTX DisplayPort path as the primary display.");
            }
            const auto normalizedConfiguration = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
            if (!normalizedConfiguration ||
                !ContainsDisplayViews(snapshot->protectedViews, *normalizedConfiguration)) {
                return fail(L"Error: normalizing the primary path changed an independent display.");
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

            if (!colorPreserved) {
                const std::wstring error = L"Error: the RTX DisplayPort path was activated, but "
                      L"the NVIDIA color settings could not be preserved: " +
                      topologyColorError;
                return fail(error);
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
        preview.dmDisplayFrequency = targetRefreshHz;
        selectedMode = preview;
    } else {
        originalControlMode = FindPreservingTargetMode(
            displayControl->deviceName.c_str(),
            *current,
            currentResolution,
            current->dmDisplayFrequency);
        selectedMode = FindPreservingTargetMode(
            displayControl->deviceName.c_str(),
            *current,
            targetResolution,
            targetRefreshHz);
    }
    if (!originalControlMode) {
        const std::wstring error =
            L"Error: the current display mode could not be validated on the "
            L"active display path; switching was cancelled.";
        return fail(error);
    }

    PrintMode(L"Current: ", *current);

    if (!selectedMode) {
        const std::wstring error =
            L"Error: " + std::to_wstring(targetResolution.width) + L'x' +
            std::to_wstring(targetResolution.height) +
            L" is not available at " +
            std::to_wstring(targetRefreshHz) + L" Hz and " +
            std::to_wstring(current->dmBitsPerPel) +
            L"-bit desktop color depth; switching was cancelled.";
        return fail(error);
    }


    NvidiaColorState originalColor;
    std::wstring colorError;
    if (!NvidiaColorControl::ReadPrimary(originalColor, colorError)) {
        return fail(L"Error: current NVIDIA color settings could not be read; switching was cancelled: " + colorError);
    }
    if (!NvidiaColorControl::IsSupportedOnPrimary(originalColor, colorError)) {
        return fail(L"Error: the NVIDIA driver did not confirm the current color settings as supported: " + colorError);
    }

    const DWORD targetDpiPercent = TargetDpiPercent(targetResolution);
    const auto displaySource = FindDatabaseDisplaySource(display->DeviceName);
    if (!displaySource) {
        return fail(L"Error: Windows could not identify the primary display's DPI source.");
    }

    const auto currentDpiScale = ReadDpiScale(*displaySource);
    if (!currentDpiScale) {
        return fail(L"Error: Windows could not read the primary display scaling range.");
    }

    const auto previewDpiRelative = RelativeDpiScaleForPercent(
        *currentDpiScale,
        targetDpiPercent);
    if (!previewDpiRelative) {
        return fail(L"Error: " + std::to_wstring(targetDpiPercent) +
            L"% scaling is not supported for the primary display.");
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
    std::wcout << (dryRun ? L"Would use power plan: " : L"Target power plan: ")
        << DescribePowerPlan(*targetPowerPlan) << L'\n';

    if (dryRun) {
        return 0;
    }

    if (!prepareRollback(*displayControl, *current, originalColor)) {
        return fail(L"Error: the original display state could not be saved; no changes made.");
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
        std::wcerr
            << L"Warning: legacy mode preflight failed: "
            << DisplayChangeError(testResult)
            << L" Trying the enumerated mode and checking the actual result.\n";
    }

    rollback.Arm();
    const LONG applyResult = ChangeDisplaySettingsExW(
        displayControl->deviceName.c_str(),
        &target,
        nullptr,
        CDS_UPDATEREGISTRY,
        nullptr);

    if (applyResult != DISP_CHANGE_SUCCESSFUL) {
        return fail(L"Error: " + std::wstring{DisplayChangeError(applyResult)});
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
        appliedMode->dmDisplayFrequency == targetRefreshHz &&
        appliedMode->dmBitsPerPel == current->dmBitsPerPel &&
        appliedMode->dmDisplayOrientation == current->dmDisplayOrientation &&
        (appliedMode->dmDisplayFlags & DM_INTERLACED) == (current->dmDisplayFlags & DM_INTERLACED);

    if (!colorPreserved || !modePreserved) {
        if (!modePreserved) {
            return fail(L"Error: the driver did not retain the requested display mode, refresh rate or color depth.");
        }
        return fail(L"Error: the target mode cannot preserve the current NVIDIA color settings: " + colorError);
    }

    // The recommended DPI baseline can change with the active resolution, so
    // recalculate the relative scale after the display mode is applied.
    if (!SetDpiPercentAndVerify(*displaySource, targetDpiPercent)) {
        return fail(L"Error: Windows could not set and verify display scaling at " +
            std::to_wstring(targetDpiPercent) + L"%.");
    }

    const auto finalMode = ReadCurrentMode(display->DeviceName);
    const auto finalDpi = ReadDpiScale(*displaySource);
    const auto finalConfiguration = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
    NvidiaColorState finalColor;
    const bool finalStatePreserved =
        finalMode &&
        HasResolution(*finalMode, targetResolution) &&
        finalMode->dmDisplayFrequency == targetRefreshHz &&
        finalMode->dmBitsPerPel == current->dmBitsPerPel &&
        finalMode->dmDisplayOrientation == current->dmDisplayOrientation &&
        (finalMode->dmDisplayFlags & DM_INTERLACED) == (current->dmDisplayFlags & DM_INTERLACED) &&
        finalDpi && DpiPercentForRelativeScale(*finalDpi, finalDpi->currentScaleRel) == targetDpiPercent &&
        finalConfiguration && ContainsDisplayViews(snapshot->protectedViews, *finalConfiguration) &&
        NvidiaColorControl::ReadPrimary(finalColor, colorError, 20) &&
        NvidiaColorControl::Equivalent(finalColor, originalColor);
    if (!finalStatePreserved) {
        return fail(L"Error: the final display state did not retain the requested mode, scaling, NVIDIA colors or independent displays.");
    }

    if (!ActivatePowerPlan(WindowsPowerPlans(), *targetPowerPlan, powerError)) {
        const std::wstring error = L"Error: "
            L"power-plan activation failed: " + powerError;
        return fail(error);
    }

    rollback.Commit();
    switchLock.Release();
    if (!NotifySettingsChange()) {
        std::wcerr << L"Warning: Windows could not queue the scaling notification; "
            L"sign out or restart may be required.\n";
    }
    if (batteryMode) {
        ShowBatteryResult(targetPowerPlan->kind);
    } else {
        std::wcout << L"Power plan: " << DescribePowerPlan(*targetPowerPlan) << L"\nDone.\n";
        WaitWhenOpenedDirectly(openedDirectly, true);
    }
    return 0;
}

int wmain(const int argc, wchar_t* argv[]) {
    try {
        return RunProgram(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Error: unexpected failure: " << error.what() << '\n';
        return 1;
    }
}
