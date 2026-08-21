#include <windows.h>

#include <iostream>
#include <optional>
#include <string_view>
#include <tuple>

namespace {

constexpr DWORD kFullHdWidth = 1720;
constexpr DWORD kFullHdHeight = 1080;
constexpr DWORD kUltraHdWidth = 3840;
constexpr DWORD kUltraHdHeight = 2160;

struct Resolution {
    DWORD width;
    DWORD height;
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

std::optional<DEVMODEW> FindBestTargetMode(
    const wchar_t* deviceName,
    const DEVMODEW& current,
    const Resolution targetResolution) {
    std::optional<DEVMODEW> best;

    const auto rank = [&current](const DEVMODEW& mode) {
        const bool sameColorDepth =
            current.dmBitsPerPel == 0 || mode.dmBitsPerPel == current.dmBitsPerPel;
        const bool sameRefreshRate =
            current.dmDisplayFrequency > 1 &&
            mode.dmDisplayFrequency == current.dmDisplayFrequency;

        // Preserve color depth first, then preserve the current refresh rate.
        // If an exact refresh-rate match is unavailable, prefer the highest one.
        return std::tuple{
            sameColorDepth,
            sameRefreshRate,
            mode.dmDisplayFrequency,
            mode.dmBitsPerPel
        };
    };

    for (DWORD index = 0;; ++index) {
        DEVMODEW candidate{};
        candidate.dmSize = sizeof(candidate);

        if (!EnumDisplaySettingsExW(deviceName, index, &candidate, 0)) {
            break;
        }

        if (!HasResolution(candidate, targetResolution)) {
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

        if (!best || rank(candidate) > rank(*best)) {
            best = candidate;
        }
    }

    return best;
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
        << L"Double-click the executable to toggle the primary display between\n"
        << L"1920x1080 and 3840x2160.\n\n"
        << L"Options:\n"
        << L"  --dry-run  Show the selected mode without changing the display.\n"
        << L"  --help     Show this help text.\n";
}

} // namespace

int wmain(const int argc, wchar_t* argv[]) {
    const bool openedDirectly = argc == 1;
    bool dryRun = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (argument == L"--dry-run") {
            dryRun = true;
        } else if (argument == L"--help" || argument == L"-h") {
            PrintHelp();
            return 0;
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

    const Resolution targetResolution = HasResolution(
        *current,
        {kUltraHdWidth, kUltraHdHeight})
        ? Resolution{kFullHdWidth, kFullHdHeight}
        : Resolution{kUltraHdWidth, kUltraHdHeight};

    const auto selectedMode = FindBestTargetMode(
        display->DeviceName,
        *current,
        targetResolution);

    PrintMode(L"Current: ", *current);

    if (!selectedMode) {
        std::wcerr
            << L"Error: no usable " << targetResolution.width << L'x'
            << targetResolution.height << L" graphics mode was found.\n";
        WaitWhenOpenedDirectly(openedDirectly, false);
        return 1;
    }

    PrintMode(dryRun ? L"Would switch to: " : L"Switching to: ", *selectedMode);

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

    std::wcout << L"Done.\n";
    WaitWhenOpenedDirectly(openedDirectly, true);
    return 0;
}
