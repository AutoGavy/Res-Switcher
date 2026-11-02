#pragma once

#include <windows.h>

#include <optional>
#include <vector>

struct DisplayConfiguration {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

// Query callbacks make hot-plug races testable without changing a real display.
std::optional<DisplayConfiguration> ReadDisplayConfiguration(
    UINT32 flags,
    decltype(&GetDisplayConfigBufferSizes) sizes = &GetDisplayConfigBufferSizes,
    decltype(&QueryDisplayConfig) query = &QueryDisplayConfig);

bool ValidDisplayModeIndex(
    const DisplayConfiguration& configuration, UINT32 index,
    DISPLAYCONFIG_MODE_INFO_TYPE type, const LUID& adapter, UINT32 id);

bool RestorableDisplayConfiguration(const DisplayConfiguration& configuration);

// Replace the primary clone group, retaining every other independent display.
std::optional<DisplayConfiguration> ReplacePrimaryDisplayPaths(
    const DisplayConfiguration& active,
    const LUID& replacedAdapter, UINT32 replacedSource,
    const DISPLAYCONFIG_PATH_INFO& replacement,
    const DISPLAYCONFIG_MODE_INFO& source,
    const DISPLAYCONFIG_MODE_INFO& target);

LONG ApplyDisplayConfiguration(
    const DisplayConfiguration& configuration, bool persist, bool allowChanges);

bool EquivalentDisplayConfigurations(
    const DisplayConfiguration& expected, const DisplayConfiguration& actual);

// Desktop adjacency may legitimately move when the primary view is resized;
// independent monitors must still retain their mode, rotation and refresh.
bool ContainsDisplayViews(
    const DisplayConfiguration& expectedViews, const DisplayConfiguration& actual);
