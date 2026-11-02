#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

enum class PowerPlanKind { Balanced, UltimatePerformance };

constexpr PowerPlanKind PowerPlanForResolution(DWORD width, DWORD height) {
    return width == 3840 && height == 2160
        ? PowerPlanKind::Balanced
        : PowerPlanKind::UltimatePerformance;
}

struct PowerScheme {
    GUID id{};
    std::wstring name;
};

struct PowerPlan {
    GUID id{};
    PowerPlanKind kind{};
};

// Keep the operating-system calls separate so tests never change real plans.
class PowerPlanBackend {
public:
    virtual ~PowerPlanBackend() = default;
    virtual DWORD Enumerate(std::vector<PowerScheme>& schemes) = 0;
    virtual DWORD GetActive(GUID& id) = 0;
    virtual DWORD SetActive(const GUID& id) = 0;
};

PowerPlanBackend& WindowsPowerPlans();
const wchar_t* PowerPlanLabel(PowerPlanKind kind);
std::wstring DescribePowerPlan(const PowerPlan& plan);
std::optional<PowerPlan> FindPowerPlan(
    PowerPlanBackend& backend, PowerPlanKind kind, std::wstring& error);
bool ActivatePowerPlan(
    PowerPlanBackend& backend, const PowerPlan& plan, std::wstring& error);
