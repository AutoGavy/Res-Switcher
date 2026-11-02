#include "power_plan.h"

#include <objbase.h>
#include <powrprof.h>
#include <powersetting.h>

#include <string_view>
#include <utility>

namespace {

constexpr GUID kBalanced{
    0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr GUID kUltimate{
    0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

std::wstring ApiError(const wchar_t* operation, DWORD status) {
    return std::wstring{operation} + L" (Windows error " +
        std::to_wstring(status) + L").";
}

DWORD ReadSchemeName(const GUID& id, std::wstring& name) {
    DWORD bytes = 0;
    DWORD status = PowerReadFriendlyName(nullptr, &id, nullptr, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) {
        return status;
    }
    if (bytes == 0 || bytes > 65536 || bytes % sizeof(wchar_t) != 0) {
        return ERROR_INVALID_DATA;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = PowerReadFriendlyName(
        nullptr, &id, nullptr, nullptr,
        reinterpret_cast<UCHAR*>(buffer.data()), &bytes);
    if (status == ERROR_SUCCESS) {
        name = buffer.data();
    }
    return status;
}

class WindowsBackend final : public PowerPlanBackend {
public:
    DWORD Enumerate(std::vector<PowerScheme>& schemes) override {
        schemes.clear();
        for (ULONG index = 0;; ++index) {
            GUID id{};
            DWORD bytes = sizeof(id);
            const DWORD status = PowerEnumerate(
                nullptr, nullptr, nullptr, ACCESS_SCHEME, index,
                reinterpret_cast<UCHAR*>(&id), &bytes);
            if (status == ERROR_NO_MORE_ITEMS) {
                return ERROR_SUCCESS;
            }
            if (status != ERROR_SUCCESS || bytes != sizeof(id)) {
                return status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
            }
            std::wstring name;
            // A renamed/unreadable scheme may still match a standard GUID.
            ReadSchemeName(id, name);
            schemes.push_back({id, std::move(name)});
        }
    }

    DWORD GetActive(GUID& id) override {
        GUID* active = nullptr;
        DWORD status = PowerGetActiveScheme(nullptr, &active);
        if (status == ERROR_SUCCESS) {
            if (active) {
                id = *active;
            } else {
                status = ERROR_INVALID_DATA;
            }
        }
        if (active) {
            LocalFree(active);
        }
        return status;
    }

    DWORD SetActive(const GUID& id) override {
        return PowerSetActiveScheme(nullptr, &id);
    }
};

bool NameEquals(std::wstring_view name, std::wstring_view expected) {
    constexpr std::wstring_view whitespace = L" \t\r\n";
    const auto first = name.find_first_not_of(whitespace);
    if (first == std::wstring_view::npos) {
        return false;
    }
    const auto last = name.find_last_not_of(whitespace);
    name = name.substr(first, last - first + 1);
    return CompareStringOrdinal(
        name.data(), static_cast<int>(name.size()),
        expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

bool NameMatches(std::wstring_view name, PowerPlanKind kind) {
    return kind == PowerPlanKind::Balanced
        ? NameEquals(name, L"Balanced") || NameEquals(name, L"\u5e73\u8861")
        : NameEquals(name, L"Ultimate Performance") ||
            NameEquals(name, L"\u5353\u8d8a\u6027\u80fd");
}

} // namespace

PowerPlanBackend& WindowsPowerPlans() {
    static WindowsBackend backend;
    return backend;
}

const wchar_t* PowerPlanLabel(PowerPlanKind kind) {
    return kind == PowerPlanKind::Balanced ? L"Balanced" : L"Ultimate Performance";
}

std::wstring DescribePowerPlan(const PowerPlan& plan) {
    wchar_t id[39]{};
    if (StringFromGUID2(plan.id, id, ARRAYSIZE(id)) == 0) {
        return std::wstring{PowerPlanLabel(plan.kind)} + L" (GUID unavailable)";
    }
    return std::wstring{PowerPlanLabel(plan.kind)} + L" " + id;
}

std::optional<PowerPlan> FindPowerPlan(
    PowerPlanBackend& backend, PowerPlanKind kind, std::wstring& error) {
    error.clear();
    std::vector<PowerScheme> schemes;
    DWORD status = backend.Enumerate(schemes);
    if (status != ERROR_SUCCESS) {
        error = ApiError(L"Could not enumerate power plans", status);
        return std::nullopt;
    }
    GUID active{};
    status = backend.GetActive(active);
    if (status != ERROR_SUCCESS) {
        error = ApiError(L"Could not read the active power plan", status);
        return std::nullopt;
    }

    const GUID& standard = kind == PowerPlanKind::Balanced ? kBalanced : kUltimate;
    std::optional<PowerPlan> selected;
    int bestScore = 0;
    bool ambiguous = false;
    for (const auto& scheme : schemes) {
        // Prefer an explicitly named custom copy, especially if already active.
        const int score = NameMatches(scheme.name, kind)
            ? (IsEqualGUID(scheme.id, active) ? 3 : 2)
            : (IsEqualGUID(scheme.id, standard) ? 1 : 0);
        if (score > bestScore) {
            selected = PowerPlan{scheme.id, kind};
            bestScore = score;
            ambiguous = false;
        } else if (score != 0 && score == bestScore && selected &&
            !IsEqualGUID(selected->id, scheme.id)) {
            ambiguous = true;
        }
    }
    if (ambiguous) {
        error = L"Multiple equally named power plans match " + std::wstring{PowerPlanLabel(kind)} +
            L". Give the intended plan a unique matching name in Windows.";
        return std::nullopt;
    }
    if (!selected) {
        error = L"Required power plan not found: " + std::wstring{PowerPlanLabel(kind)} +
            L". Enable this plan in Windows first; no replacement plan was created.";
    }
    return selected;
}

bool ActivatePowerPlan(
    PowerPlanBackend& backend, const PowerPlan& plan, std::wstring& error) {
    error.clear();
    GUID original{};
    DWORD status = backend.GetActive(original);
    if (status != ERROR_SUCCESS) {
        error = ApiError(L"Could not read the active power plan", status);
        return false;
    }
    if (IsEqualGUID(original, plan.id)) {
        return true; // Do not reapply an already active plan.
    }
    const DWORD setStatus = backend.SetActive(plan.id);
    GUID actual{};
    status = backend.GetActive(actual);
    if (setStatus == ERROR_SUCCESS && status == ERROR_SUCCESS && IsEqualGUID(actual, plan.id)) {
        return true;
    }

    error = setStatus != ERROR_SUCCESS
        ? ApiError(L"Could not activate the requested power plan", setStatus)
        : status == ERROR_SUCCESS
            ? L"Windows did not retain the requested power plan."
            : ApiError(L"Could not verify the requested power plan", status);
    if (status == ERROR_SUCCESS && IsEqualGUID(actual, original)) {
        error += L" The previous power plan is unchanged.";
        return false; // No redundant activation/notification during recovery.
    }
    // Some policy/driver failures can be reported after a partial change.
    const DWORD restoreStatus = backend.SetActive(original);
    GUID restored{};
    if (restoreStatus == ERROR_SUCCESS && backend.GetActive(restored) == ERROR_SUCCESS &&
        IsEqualGUID(restored, original)) {
        error += L" The previous power plan was restored.";
    } else {
        error += L" Restoring the previous power plan could not be confirmed.";
    }
    return false;
}
