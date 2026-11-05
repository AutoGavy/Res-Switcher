#include <windows.h>

#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

// Exercise the NVAPI loader policy with a loader that never loads any DLL.
DWORD observedLoadFlags = 0;
HMODULE WINAPI TestLoadLibrary(LPCWSTR, HANDLE, DWORD flags) {
    observedLoadFlags = flags;
    return nullptr;
}
#define LoadLibraryExW TestLoadLibrary
#include "../nv_color_state.cpp"
#undef LoadLibraryExW

// Verify the success banner without changing a real console or waiting.
bool testingBatteryResult = false;
bool batteryConsoleAvailable = true;
bool batteryColorSucceeds = true;
constexpr WORD batteryOriginalColor = BACKGROUND_RED |
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
std::vector<WORD> batteryColors;
std::vector<DWORD> batteryWaits;

BOOL WINAPI TestConsoleInfo(HANDLE output, PCONSOLE_SCREEN_BUFFER_INFO info) {
    if (!testingBatteryResult) { return GetConsoleScreenBufferInfo(output, info); }
    *info = {};
    info->wAttributes = batteryOriginalColor;
    return batteryConsoleAvailable ? TRUE : FALSE;
}
BOOL WINAPI TestConsoleColor(HANDLE output, WORD attributes) {
    if (!testingBatteryResult) { return SetConsoleTextAttribute(output, attributes); }
    batteryColors.push_back(attributes);
    return batteryColorSucceeds ? TRUE : FALSE;
}
void WINAPI TestResultSleep(DWORD milliseconds) {
    if (!testingBatteryResult) { Sleep(milliseconds); return; }
    batteryWaits.push_back(milliseconds);
}

// Include pure mode/DPI helpers and argument handling, without using the app's
// entry point. These tests never run a display/power apply function.
#define GetConsoleScreenBufferInfo TestConsoleInfo
#define SetConsoleTextAttribute TestConsoleColor
#define Sleep TestResultSleep
#define wmain UnusedApplicationEntryPoint
#include "../main.cpp"
#undef wmain
#undef Sleep
#undef SetConsoleTextAttribute
#undef GetConsoleScreenBufferInfo

namespace {
void Check(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

unsigned int sizeCalls = 0;
unsigned int queryCalls = 0;
unsigned int insufficient = 0;
bool failSizes = false;
bool failQuery = false;
bool oversizedReply = false;
bool databasePointer = false;

LONG WINAPI FakeSizes(UINT32, UINT32* paths, UINT32* modes) {
    ++sizeCalls;
    *paths = 2;
    *modes = 4;
    return failSizes ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
LONG WINAPI FakeQuery(UINT32 flags, UINT32* paths, DISPLAYCONFIG_PATH_INFO* pathArray,
    UINT32* modes, DISPLAYCONFIG_MODE_INFO* modeArray, DISPLAYCONFIG_TOPOLOGY_ID* topology) {
    ++queryCalls;
    databasePointer = (flags & QDC_DATABASE_CURRENT) == 0 ? topology == nullptr : topology != nullptr;
    Check(pathArray != nullptr && modeArray != nullptr, "Non-null CCD buffers");
    if (queryCalls <= insufficient) { return ERROR_INSUFFICIENT_BUFFER; }
    if (failQuery) { return ERROR_ACCESS_DENIED; }
    *paths = oversizedReply ? 10 : 1;
    *modes = 0;
    return ERROR_SUCCESS;
}
void ResetQuery() {
    sizeCalls = queryCalls = insufficient = 0;
    failSizes = failQuery = oversizedReply = false;
}

void TestQueryRetries() {
    ResetQuery();
    insufficient = 2;
    const auto result = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS, FakeSizes, FakeQuery);
    Check(result && result->paths.size() == 1 && result->modes.empty(), "Resize to valid rows");
    Check(sizeCalls == 3 && queryCalls == 3 && databasePointer, "Retry hot-plug races");
    ResetQuery();
    insufficient = 10;
    Check(!ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS, FakeSizes, FakeQuery), "Bounded retries");
    Check(queryCalls == 4, "Never spin indefinitely on hot-plug");
    ResetQuery();
    failSizes = true;
    Check(!ReadDisplayConfiguration(QDC_ALL_PATHS, FakeSizes, FakeQuery) && queryCalls == 0, "Size failure");
    ResetQuery();
    failQuery = true;
    Check(!ReadDisplayConfiguration(QDC_ALL_PATHS, FakeSizes, FakeQuery) && queryCalls == 1, "Query failure");
    ResetQuery();
    oversizedReply = true;
    Check(!ReadDisplayConfiguration(QDC_ALL_PATHS, FakeSizes, FakeQuery), "Reject invalid output counts");
    ResetQuery();
    Check(ReadDisplayConfiguration(QDC_DATABASE_CURRENT, FakeSizes, FakeQuery).has_value() &&
        databasePointer, "Database queries require a topology output pointer");
}

void AddPath(DisplayConfiguration& configuration, DWORD adapter, UINT32 source, UINT32 target, LONG x) {
    DISPLAYCONFIG_MODE_INFO a{};
    a.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    a.adapterId.LowPart = adapter;
    a.id = source;
    a.sourceMode.width = 3840;
    a.sourceMode.height = 2160;
    a.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    a.sourceMode.position.x = x;
    DISPLAYCONFIG_MODE_INFO b{};
    b.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    b.adapterId = a.adapterId;
    b.id = target;
    DISPLAYCONFIG_PATH_INFO path{};
    path.sourceInfo.adapterId = a.adapterId;
    path.sourceInfo.id = source;
    path.sourceInfo.modeInfoIdx = static_cast<UINT32>(configuration.modes.size());
    path.targetInfo.adapterId = b.adapterId;
    path.targetInfo.id = target;
    path.targetInfo.modeInfoIdx = path.sourceInfo.modeInfoIdx + 1;
    path.targetInfo.refreshRate = {240, 1};
    path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    configuration.paths.push_back(path);
    configuration.modes.push_back(a);
    configuration.modes.push_back(b);
}

void TestTopologyPreservation() {
    DisplayConfiguration original;
    AddPath(original, 1, 0, 10, 0); // Primary clone group.
    AddPath(original, 2, 1, 20, 3840); // Independent monitor.
    DisplayConfiguration replacement;
    AddPath(replacement, 3, 0, 30, 0);
    replacement.modes[0].sourceMode.width = 2560;
    const auto replace = [&](const DisplayConfiguration& source) {
        return ReplacePrimaryDisplayPaths(source, original.paths[0].sourceInfo.adapterId, 0,
            replacement.paths[0], replacement.modes[0], replacement.modes[1]);
    };
    const auto updated = replace(original);
    Check(updated && updated->paths.size() == 2 && updated->modes.size() == 4, "Keep independent monitors");
    Check(updated->paths[0].targetInfo.id == 30 && updated->paths[1].targetInfo.id == 20,
        "Replace only the primary group");
    Check(updated->modes[updated->paths[1].sourceInfo.modeInfoIdx].sourceMode.position.x == 3840,
        "Preserve the other monitor position");
    Check(RestorableDisplayConfiguration(*updated), "Rebased indices and metadata remain coherent");
    auto invalid = original;
    invalid.paths[1].targetInfo.modeInfoIdx = 0xffff;
    Check(!replace(invalid), "Reject out-of-range mode indices");
    invalid = original;
    invalid.modes[3].infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    Check(!replace(invalid), "Reject wrong mode types");
    invalid = original;
    invalid.modes[3].id = 99;
    Check(!replace(invalid), "Reject wrong target identity");
    invalid = original;
    invalid.paths[1].sourceInfo.adapterId = replacement.paths[0].sourceInfo.adapterId;
    invalid.paths[1].sourceInfo.id = 0;
    Check(!replace(invalid), "Do not clone an independent monitor onto the replacement source");
    Check(EquivalentDisplayConfigurations(original, original), "Verify exact topology snapshot");
    DisplayConfiguration protectedView = original;
    protectedView.paths.erase(protectedView.paths.begin());
    Check(ContainsDisplayViews(protectedView, *updated), "Retain independent views across primary-source replacement");
    invalid = original;
    invalid.modes[2].sourceMode.width = 1920;
    Check(!EquivalentDisplayConfigurations(original, invalid), "Detect incomplete mode restoration");
    Check(!ContainsDisplayViews(protectedView, invalid), "Detect unexpected secondary resolution changes");
    invalid = original;
    invalid.modes[2].sourceMode.position.x = 1920;
    Check(ContainsDisplayViews(protectedView, invalid), "Permit normal desktop adjacency changes");
    invalid.paths[1].targetInfo.refreshRate = {180, 1};
    Check(!ContainsDisplayViews(protectedView, invalid), "Never downgrade another monitor's refresh rate");
    invalid = original;
    invalid.paths.pop_back();
    Check(!EquivalentDisplayConfigurations(original, invalid), "Detect a disconnected secondary monitor");
    invalid = original;
    std::swap(invalid.paths[0], invalid.paths[1]);
    Check(EquivalentDisplayConfigurations(original, invalid), "Indices, not path order, identify restored views");
    invalid.paths[1] = invalid.paths[0];
    Check(!EquivalentDisplayConfigurations(original, invalid), "A path cannot satisfy two expected views");

    // Two remaining cloned targets must retain their shared source row.
    AddPath(original, 2, 1, 21, 3840);
    original.paths.back().sourceInfo.modeInfoIdx = 2;
    const auto cloned = replace(original);
    Check(cloned && cloned->paths.size() == 3 && cloned->modes.size() == 5,
        "Deduplicate shared source modes for independent clones");
    Check(cloned->paths[1].sourceInfo.modeInfoIdx == cloned->paths[2].sourceInfo.modeInfoIdx,
        "Retain clone associations");
}

void TestRollbackAndLock() {
    unsigned int count = 0;
    { RollbackAction action([&] { ++count; }); }
    Check(count == 0, "Preflight must not roll back an untouched display");
    { RollbackAction action([&] { ++count; }); action.Arm(); }
    Check(count == 1, "Armed rollback runs on early return");
    { RollbackAction action([&] { ++count; }); action.Arm(); action.Run(); }
    Check(count == 2, "Explicit error recovery is not repeated by the destructor");
    { RollbackAction action([&] { ++count; }); action.Arm(); action.Commit(); }
    Check(count == 2, "Successful display changes are not reverted");
    try { RollbackAction action([&] { ++count; }); action.Arm(); throw std::runtime_error("test"); }
    catch (const std::exception&) {}
    Check(count == 3, "Recover during exception unwinding");

    const std::wstring name = L"Local\\ResSwitcher.SafetyTests." + std::to_wstring(GetCurrentProcessId());
    SwitchLock owner(name.c_str());
    Check(owner.Acquired(), "Acquire the switch lock");
    bool rejected = false;
    std::thread contender([&] { SwitchLock lock(name.c_str()); rejected = !lock.Acquired(); });
    contender.join();
    Check(rejected, "Reject a concurrent switch");
    owner.Release();
    bool acquired = false;
    std::thread successor([&] { SwitchLock lock(name.c_str()); acquired = lock.Acquired(); });
    successor.join();
    Check(acquired, "Release the lock before displaying completion/error UI");
}

void TestDpiAndColors() {
    const DpiScaleInfo scale{-2, 0, 9};
    Check(DpiPercentForRelativeScale(scale, 0) == DWORD{150}, "Read absolute DPI percentage");
    Check(RelativeDpiScaleForPercent(scale, 125) == -1, "Calculate relative DPI baseline");
    Check(!RelativeDpiScaleForPercent(scale, 130), "Reject unsupported custom percentage");
    const DpiScaleInfo changedBaseline{-1, 0, 10};
    Check(RelativeDpiScaleForPercent(changedBaseline, 150) == 1,
        "Recalculate relative DPI when restoring an absolute percentage");
    const DpiScaleInfo invalid{(std::numeric_limits<LONG>::min)(), 0, 0};
    Check(!DpiPercentForRelativeScale(invalid, 0) && !RelativeDpiScaleForPercent(invalid, 125),
        "Reject malformed DPI ranges without signed overflow");
    NvidiaColorState known;
    known.hasDesktopDepth = true;
    known.desktopDepth = 1;
    auto unknown = known;
    unknown.hasDesktopDepth = false;
    Check(!NvidiaColorControl::Equivalent(known, unknown), "Unknown depth cannot prove color preservation");
    Check(!NvidiaColorControl::Equivalent(unknown, known), "Color comparison remains symmetric");
    Check(NvidiaColorControl::Equivalent(known, known), "Identical colors compare equal");
    known.hasColorSelectionPolicy = true;
    known.colorSelectionPolicy = 1; // Driver-selected best quality.
    const auto data = MakeColorDataV5(kColorCommandSet, &known);
    Check(data.data.colorSelectionPolicy == 1, "Do not force automatic color selection into manual policy");
    const auto roundTrip = ToState(data);
    Check(roundTrip.hasColorSelectionPolicy && roundTrip.colorSelectionPolicy == 1,
        "Capture the original color selection policy");
    unknown = known;
    unknown.colorSelectionPolicy = 0;
    Check(!NvidiaColorControl::Equivalent(known, unknown), "Verify the original automatic/manual color policy");
    std::wstring error;
    NvApiSession session;
    Check(!session.Open(error) && observedLoadFlags == LOAD_LIBRARY_SEARCH_SYSTEM32,
        "Never load NVAPI from the application directory or current directory");
}

void TestBatteryResult() {
    const auto capture = [](PowerPlanKind kind, bool hasConsole, bool colorSucceeds) {
        batteryColors.clear();
        batteryWaits.clear();
        batteryConsoleAvailable = hasConsole;
        batteryColorSucceeds = colorSucceeds;
        std::wostringstream output;
        auto* original = std::wcout.rdbuf(output.rdbuf());
        testingBatteryResult = true;
        ShowBatteryResult(kind);
        testingBatteryResult = false;
        std::wcout.rdbuf(original);
        return output.str();
    };
    for (const auto kind : {PowerPlanKind::Balanced, PowerPlanKind::UltimatePerformance}) {
        const auto text = capture(kind, true, true);
        const std::wstring expected = std::wstring{L"Current power plan: "} + PowerPlanLabel(kind);
        Check(text.find(expected) != std::wstring::npos, "English success label and active plan name");
        Check(text.find(L"==================================================") != std::wstring::npos,
            "Prominent success banner");
        Check(batteryColors == std::vector<WORD>{
            BACKGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, batteryOriginalColor},
            "Highlight only the plan in bright green, preserve background, restore original color");
        Check(batteryWaits == std::vector<DWORD>{1000}, "Hold the flushed success result for exactly 1 second");
    }
    const auto redirected = capture(PowerPlanKind::Balanced, false, true);
    Check(redirected.find(L"Current power plan: Balanced") != std::wstring::npos && batteryColors.empty(),
        "Redirected output stays readable without color calls or escape sequences");
    capture(PowerPlanKind::Balanced, true, false);
    Check(batteryColors.size() == 1 && batteryWaits == std::vector<DWORD>{1000},
        "Color failure still displays the result and pauses");
}

void TestBatteryArguments() {
    const auto rejected = [](std::initializer_list<const wchar_t*> arguments) {
        std::vector<std::wstring> storage{L"Res Switcher.exe", L"--dry-run"};
        for (const auto* argument : arguments) { storage.emplace_back(argument); }
        std::vector<wchar_t*> argv;
        for (auto& argument : storage) { argv.push_back(argument.data()); }
        // Even if argument validation regresses, the extra --dry-run prevents
        // these tests from ever changing the real display or power plan.
        return RunProgram(static_cast<int>(argv.size()), argv.data()) == 2;
    };
    Check(rejected({L"-battery", L"-battery"}), "Reject duplicate battery mode");
    Check(rejected({L"-battery", L"--restore-4k"}), "Reject battery plus explicit restore");
    Check(rejected({L"--restore-4k", L"-battery"}), "Reject restore plus battery in either order");
    Check(rejected({L"-restore4K", L"-battery"}), "Restore alias cannot toggle power plans");
    Check(rejected({L"--restore-4k", L"-restore4K"}), "Reject duplicate restore aliases");
    Check(rejected({L"-restore4K", L"-restore4K"}), "Reject repeated restore alias");
    for (const auto* resolution : {L"-1050", L"-1080", L"-1440"}) {
        Check(rejected({L"-battery", resolution}), "Reject battery plus resolution");
        Check(rejected({resolution, L"-battery"}), "Reject resolution plus battery in either order");
        Check(rejected({L"-restore4K", resolution}), "Restore alias cannot toggle resolutions");
    }
}
}

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--inspect") {
            const auto active = ReadDisplayConfiguration(QDC_ONLY_ACTIVE_PATHS);
            const auto saved = ReadDisplayConfiguration(QDC_DATABASE_CURRENT);
            std::cout << "Read-only CCD inspection: active=" <<
                (active && RestorableDisplayConfiguration(*active)) << ", saved=" <<
                (saved && RestorableDisplayConfiguration(*saved)) << '\n';
            const auto display = FindPrimaryDisplay();
            const auto mode = display ? ReadCurrentMode(display->DeviceName) : std::nullopt;
            const auto control = display && mode ? FindDisplayControl(*display, *mode) : std::nullopt;
            const auto snapshot = control && mode ? CaptureDisplaySnapshot(*control, *mode, {}) : std::nullopt;
            std::cout << "Original-state capture available=" << snapshot.has_value() << '\n';
            return active && saved && snapshot ? 0 : 1;
        }
        TestQueryRetries();
        TestTopologyPreservation();
        TestRollbackAndLock();
        TestDpiAndColors();
        TestBatteryArguments();
        TestBatteryResult();
        std::cout << "Safety tests passed (mock CCD/DLL calls; no display or power changes).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
