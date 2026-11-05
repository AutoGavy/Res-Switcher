#include "../power_plan.h"

#include <iostream>
#include <stdexcept>

namespace {

constexpr GUID kUltimateCopy{0x11111111, 0, 0, {}};
constexpr GUID kHighPerformance{0x22222222, 0, 0, {}};
constexpr GUID kBalancedCopy{0x33333333, 0, 0, {}};
constexpr GUID kAnotherCopy{0x44444444, 0, 0, {}};
constexpr GUID kStandardBalanced{
    0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr GUID kStandardUltimate{
    0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

static_assert(PowerPlanForResolution(3840, 2160) == PowerPlanKind::Balanced);
static_assert(PowerPlanForResolution(2560, 1440) == PowerPlanKind::UltimatePerformance);
static_assert(PowerPlanForResolution(1920, 1080) == PowerPlanKind::UltimatePerformance);
static_assert(PowerPlanForResolution(1680, 1050) == PowerPlanKind::UltimatePerformance);
static_assert(PowerPlanForResolution(1720, 1080) == PowerPlanKind::UltimatePerformance);

class FakeBackend final : public PowerPlanBackend {
public:
    std::vector<PowerScheme> schemes{
        {kUltimateCopy, L"\u5353\u8d8a\u6027\u80fd"},
        {kHighPerformance, L"High performance"},
        {kBalancedCopy, L"\u5e73\u8861"}};
    GUID active = kHighPerformance;
    DWORD enumerateError = ERROR_SUCCESS;
    unsigned int reads = 0;
    unsigned int failRead = 0;
    unsigned int failSet = 0;
    bool ignoreFirstSet = false;
    bool changeDespiteError = false;
    bool wrongFirstSet = false;
    std::vector<GUID> writes;

    DWORD Enumerate(std::vector<PowerScheme>& output) override {
        output = schemes;
        return enumerateError;
    }
    DWORD GetActive(GUID& output) override {
        if (++reads == failRead) {
            return ERROR_ACCESS_DENIED;
        }
        output = active;
        return ERROR_SUCCESS;
    }
    DWORD SetActive(const GUID& id) override {
        writes.push_back(id);
        if (writes.size() == failSet) {
            if (changeDespiteError) { active = id; }
            return ERROR_ACCESS_DENIED;
        }
        if (wrongFirstSet && writes.size() == 1) {
            active = kAnotherCopy;
            return ERROR_SUCCESS;
        }
        if (!ignoreFirstSet || writes.size() != 1) {
            active = id;
        }
        return ERROR_SUCCESS;
    }
};

void Require(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error{description};
    }
}

void TestLookup() {
    std::wstring error;
    FakeBackend api;
    const auto balanced = FindPowerPlan(api, PowerPlanKind::Balanced, error);
    const auto ultimate = FindPowerPlan(api, PowerPlanKind::UltimatePerformance, error);
    Require(balanced && IsEqualGUID(balanced->id, kBalancedCopy), "Chinese Balanced copy");
    Require(ultimate && IsEqualGUID(ultimate->id, kUltimateCopy), "Chinese Ultimate copy");
    Require(api.writes.empty(), "Lookup must never activate a plan");
    Require(error.empty(), "Successful lookup clears the error");
    Require(DescribePowerPlan(*ultimate).find(L"Ultimate Performance {") == 0,
        "Console label must be stable English and include the GUID");

    api.schemes = {{kUltimateCopy, L"  uLtImAtE pErFoRmAnCe\t"},
        {kBalancedCopy, L"balanced"}};
    Require(FindPowerPlan(api, PowerPlanKind::Balanced, error).has_value(), "English Balanced");
    Require(FindPowerPlan(api, PowerPlanKind::UltimatePerformance, error).has_value(),
        "English Ultimate, case-insensitive with whitespace");

    api.schemes = {{kStandardBalanced, L"Renamed"}, {kStandardUltimate, L"Renamed"}};
    Require(FindPowerPlan(api, PowerPlanKind::Balanced, error).has_value(), "Balanced GUID fallback");
    Require(FindPowerPlan(api, PowerPlanKind::UltimatePerformance, error).has_value(),
        "Ultimate GUID fallback");
    api.schemes.push_back({kBalancedCopy, L"Balanced"});
    Require(IsEqualGUID(FindPowerPlan(api, PowerPlanKind::Balanced, error)->id, kBalancedCopy),
        "Named custom copy takes priority over a renamed standard GUID");
    api.schemes.push_back({kAnotherCopy, L"Balanced"});
    Require(!FindPowerPlan(api, PowerPlanKind::Balanced, error), "Ambiguous plan names must not pick an arbitrary copy");
    api.active = kAnotherCopy;
    Require(IsEqualGUID(FindPowerPlan(api, PowerPlanKind::Balanced, error)->id, kAnotherCopy),
        "Prefer an already active matching copy");

    api.schemes = {{kHighPerformance, L"High performance"}};
    Require(!FindPowerPlan(api, PowerPlanKind::UltimatePerformance, error),
        "High performance must not substitute for Ultimate");
    Require(!error.empty() && api.writes.empty(), "Missing plan must fail without writes");
    api.schemes.clear();
    Require(!FindPowerPlan(api, PowerPlanKind::Balanced, error), "Empty enumeration");
    api.enumerateError = ERROR_ACCESS_DENIED;
    Require(!FindPowerPlan(api, PowerPlanKind::Balanced, error), "Enumeration failure");
    api.enumerateError = ERROR_SUCCESS;
    api.failRead = api.reads + 1;
    Require(!FindPowerPlan(api, PowerPlanKind::Balanced, error), "Active plan read failure");
}

void TestToggle() {
    std::wstring error = L"stale error";
    {
        FakeBackend api;
        api.active = kBalancedCopy;
        auto next = FindToggledPowerPlan(api, error);
        Require(next && next->kind == PowerPlanKind::UltimatePerformance &&
            IsEqualGUID(next->id, kUltimateCopy), "Balanced toggles to Ultimate");
        Require(api.reads == 1 && api.writes.empty() && error.empty(),
            "Resolve from one original-plan snapshot without activating");
        Require(ActivatePowerPlan(api, *next, error), "Apply Balanced-to-Ultimate toggle");
        next = FindToggledPowerPlan(api, error);
        Require(next && next->kind == PowerPlanKind::Balanced &&
            IsEqualGUID(next->id, kBalancedCopy), "Ultimate toggles back to Balanced");
        Require(ActivatePowerPlan(api, *next, error), "Apply Ultimate-to-Balanced toggle");
        Require(api.writes.size() == 2 && IsEqualGUID(api.active, kBalancedCopy),
            "Two toggles return to the original plan with no intermediate activation");
    }
    {
        FakeBackend api;
        const auto next = FindToggledPowerPlan(api, error);
        Require(next && next->kind == PowerPlanKind::Balanced,
            "Other active plans switch to Balanced");
        Require(api.writes.empty(), "Preview never activates a plan");
        api.active = kAnotherCopy;
        Require(FindToggledPowerPlan(api, error)->kind == PowerPlanKind::Balanced,
            "Unknown active GUID switches to Balanced");
    }
    {
        FakeBackend api;
        api.schemes = {{kStandardBalanced, L"Renamed"}, {kStandardUltimate, L"Renamed"}};
        api.active = kStandardBalanced;
        const auto next = FindToggledPowerPlan(api, error);
        Require(next && IsEqualGUID(next->id, kStandardUltimate), "Recognize standard Balanced GUID");
        api.active = kStandardUltimate;
        Require(IsEqualGUID(FindToggledPowerPlan(api, error)->id, kStandardBalanced),
            "Recognize standard Ultimate GUID");
        api.schemes = {{kBalancedCopy, L"  bALanCED\t"}, {kUltimateCopy, L"Ultimate Performance"}};
        api.active = kBalancedCopy;
        Require(IsEqualGUID(FindToggledPowerPlan(api, error)->id, kUltimateCopy),
            "Recognize English custom plans with whitespace and case folding");
    }
    {
        FakeBackend api;
        api.schemes.push_back({kAnotherCopy, L"Balanced"});
        Require(!FindToggledPowerPlan(api, error) && api.writes.empty(),
            "Reject ambiguous destination names before changing anything");
        api.active = kAnotherCopy;
        const auto next = FindToggledPowerPlan(api, error);
        Require(next && IsEqualGUID(next->id, kUltimateCopy),
            "Active Balanced copy is recognized even with duplicate source names");
        api.schemes = {{kBalancedCopy, L"Balanced"}};
        api.active = kBalancedCopy;
        Require(!FindToggledPowerPlan(api, error) && !error.empty() && api.writes.empty(),
            "Missing Ultimate cancels Balanced-to-Ultimate toggle");
        api.schemes = {{kUltimateCopy, L"Ultimate Performance"}};
        api.active = kUltimateCopy;
        Require(!FindToggledPowerPlan(api, error) && api.writes.empty(),
            "Missing Balanced cancels Ultimate-to-Balanced toggle");
    }
    {
        FakeBackend api;
        api.enumerateError = ERROR_ACCESS_DENIED;
        Require(!FindToggledPowerPlan(api, error) && api.reads == 0 && api.writes.empty(),
            "Enumeration failure cancels toggle without activation");
        api.enumerateError = ERROR_SUCCESS;
        api.failRead = 1;
        Require(!FindToggledPowerPlan(api, error) && api.writes.empty(),
            "Unreadable original plan cancels toggle without activation");
    }
}

void TestRestoreBalanced() {
    FakeBackend api;
    api.active = kUltimateCopy;
    std::wstring error;
    const auto target = FindPowerPlan(api, PowerPlanKind::Balanced, error);
    Require(target && IsEqualGUID(target->id, kBalancedCopy),
        "Restore 4K always selects Balanced even when Ultimate is active");
    Require(api.writes.empty(), "Restore preview does not activate Balanced");
    Require(ActivatePowerPlan(api, *target, error), "Restore the Balanced plan");
    Require(IsEqualGUID(api.active, kBalancedCopy) && api.writes.size() == 1,
        "Restore activates Balanced exactly once");
    Require(ActivatePowerPlan(api, *target, error) && api.writes.size() == 1,
        "Repeated restore keeps Balanced instead of toggling back to Ultimate");
}

void TestActivation() {
    const PowerPlan plan{kBalancedCopy, PowerPlanKind::Balanced};
    std::wstring error;
    {
        FakeBackend api;
        api.active = plan.id;
        Require(ActivatePowerPlan(api, plan, error), "Already active succeeds");
        Require(api.writes.empty(), "Already active must not be reapplied");
    }
    {
        FakeBackend api;
        Require(ActivatePowerPlan(api, plan, error), "Activation succeeds");
        Require(api.writes.size() == 1 && IsEqualGUID(api.active, plan.id), "Activate exactly once");
        Require(api.reads == 2, "Verify the active plan after setting it");
    }
    {
        FakeBackend api;
        api.failRead = 1;
        Require(!ActivatePowerPlan(api, plan, error), "Cannot read original plan");
        Require(api.writes.empty(), "Do not activate without a readable original plan");
    }
    {
        FakeBackend api;
        api.failSet = 1;
        Require(!ActivatePowerPlan(api, plan, error), "SetActive failure");
        Require(IsEqualGUID(api.active, kHighPerformance), "Rejected activation preserves original");
    }
    {
        FakeBackend api;
        api.ignoreFirstSet = true;
        Require(!ActivatePowerPlan(api, plan, error), "Successful API status but wrong actual plan");
        Require(api.writes.size() == 1 && IsEqualGUID(api.active, kHighPerformance),
            "Do not reapply the original plan if it was never changed");
        Require(error.find(L"previous power plan is unchanged") != std::wstring::npos,
            "Report verified unchanged state");
    }
    {
        FakeBackend api;
        api.failRead = 2;
        Require(!ActivatePowerPlan(api, plan, error), "Post-activation verification read failure");
        Require(IsEqualGUID(api.active, kHighPerformance), "Restore after failed verification read");
    }
    {
        FakeBackend api;
        api.wrongFirstSet = true;
        api.failSet = 2;
        Require(!ActivatePowerPlan(api, plan, error), "Restoration failure");
        Require(error.find(L"could not be confirmed") != std::wstring::npos,
            "Do not claim restoration succeeded");
    }
    {
        FakeBackend api;
        api.failSet = 1;
        api.changeDespiteError = true;
        Require(!ActivatePowerPlan(api, plan, error), "Activation can fail after partially changing state");
        Require(api.writes.size() == 2 && IsEqualGUID(api.active, kHighPerformance),
            "Recover partial activation even when SetActive returned an error");
    }
}

} // namespace

int main() {
    try {
        TestLookup();
        TestToggle();
        TestRestoreBalanced();
        TestActivation();
        std::cout << "Power plan tests passed (mock backend; no system changes).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return 1;
    }
}
