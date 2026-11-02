#include "display_config.h"

#include <utility>

namespace {
bool SameAdapterId(const LUID& left, const LUID& right) {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}
}

std::optional<DisplayConfiguration> ReadDisplayConfiguration(
    const UINT32 flags, decltype(&GetDisplayConfigBufferSizes) sizes,
    decltype(&QueryDisplayConfig) query) {
    for (unsigned int attempt = 0; attempt < 4; ++attempt) {
        UINT32 paths = 0;
        UINT32 modes = 0;
        if (sizes(flags, &paths, &modes) != ERROR_SUCCESS || paths == 0 ||
            paths > 16384 || modes > 65536) {
            return std::nullopt;
        }
        // QueryDisplayConfig requires non-null arrays, even with no mode rows.
        DisplayConfiguration result;
        result.paths.resize(paths);
        result.modes.resize(modes == 0 ? 1 : modes);
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        const LONG status = query(flags, &paths, result.paths.data(), &modes,
            result.modes.data(), (flags & QDC_DATABASE_CURRENT) != 0 ? &topology : nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            continue; // A display was added between the size and data queries.
        }
        if (status != ERROR_SUCCESS || paths > result.paths.size() ||
            modes > result.modes.size()) {
            return std::nullopt;
        }
        result.paths.resize(paths);
        result.modes.resize(modes);
        return result;
    }
    return std::nullopt;
}

bool ValidDisplayModeIndex(
    const DisplayConfiguration& configuration, const UINT32 index,
    const DISPLAYCONFIG_MODE_INFO_TYPE type, const LUID& adapter, const UINT32 id) {
    return index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
        index < configuration.modes.size() &&
        configuration.modes[index].infoType == type &&
        SameAdapterId(configuration.modes[index].adapterId, adapter) &&
        configuration.modes[index].id == id;
}

bool RestorableDisplayConfiguration(const DisplayConfiguration& configuration) {
    if (configuration.paths.empty()) { return false; }
    for (const auto& path : configuration.paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 ||
            !ValidDisplayModeIndex(configuration, path.sourceInfo.modeInfoIdx,
                DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, path.sourceInfo.adapterId, path.sourceInfo.id) ||
            !ValidDisplayModeIndex(configuration, path.targetInfo.modeInfoIdx,
                DISPLAYCONFIG_MODE_INFO_TYPE_TARGET, path.targetInfo.adapterId, path.targetInfo.id)) {
            return false;
        }
    }
    return true;
}

std::optional<DisplayConfiguration> ReplacePrimaryDisplayPaths(
    const DisplayConfiguration& active,
    const LUID& replacedAdapter, const UINT32 replacedSource,
    const DISPLAYCONFIG_PATH_INFO& replacement,
    const DISPLAYCONFIG_MODE_INFO& source,
    const DISPLAYCONFIG_MODE_INFO& target) {
    DisplayConfiguration result;
    // Reindex only referenced modes. Unreferenced/duplicate source rows can
    // otherwise cause SetDisplayConfig to reject the supplied configuration.
    std::vector<UINT32> remapped(active.modes.size(), DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
    const auto copyMode = [&](const UINT32 index) {
        if (remapped[index] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
            remapped[index] = static_cast<UINT32>(result.modes.size());
            result.modes.push_back(active.modes[index]);
        }
        return remapped[index];
    };
    auto primary = replacement;
    primary.sourceInfo.modeInfoIdx = 0;
    primary.targetInfo.modeInfoIdx = 1;
    result.paths.push_back(primary); // The primary view retains first priority.
    result.modes = {source, target};

    for (auto path : active.paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
            continue;
        }
        if ((SameAdapterId(path.sourceInfo.adapterId, replacedAdapter) &&
                path.sourceInfo.id == replacedSource) ||
            (SameAdapterId(path.targetInfo.adapterId, replacement.targetInfo.adapterId) &&
                path.targetInfo.id == replacement.targetInfo.id)) {
            continue;
        }
        // Do not silently turn an independent view into a clone of the new one.
        if (SameAdapterId(path.sourceInfo.adapterId, replacement.sourceInfo.adapterId) &&
            path.sourceInfo.id == replacement.sourceInfo.id) {
            return std::nullopt;
        }
        if (!ValidDisplayModeIndex(active, path.sourceInfo.modeInfoIdx,
                DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, path.sourceInfo.adapterId, path.sourceInfo.id) ||
            !ValidDisplayModeIndex(active, path.targetInfo.modeInfoIdx,
                DISPLAYCONFIG_MODE_INFO_TYPE_TARGET, path.targetInfo.adapterId, path.targetInfo.id)) {
            return std::nullopt;
        }
        path.sourceInfo.modeInfoIdx = copyMode(path.sourceInfo.modeInfoIdx);
        path.targetInfo.modeInfoIdx = copyMode(path.targetInfo.modeInfoIdx);
        result.paths.push_back(path);
    }
    return result;
}

LONG ApplyDisplayConfiguration(
    const DisplayConfiguration& configuration, const bool persist, const bool allowChanges) {
    if (!RestorableDisplayConfiguration(configuration)) {
        return ERROR_INVALID_PARAMETER;
    }
    auto paths = configuration.paths;
    auto modes = configuration.modes;
    return SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
        static_cast<UINT32>(modes.size()), modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG |
            (persist ? SDC_SAVE_TO_DATABASE : 0) |
            (allowChanges ? SDC_ALLOW_CHANGES : 0));
}

bool EquivalentDisplayConfigurations(
    const DisplayConfiguration& expected, const DisplayConfiguration& actual) {
    if (!RestorableDisplayConfiguration(expected) || !RestorableDisplayConfiguration(actual) ||
        expected.paths.size() != actual.paths.size()) { return false; }
    std::vector<bool> used(actual.paths.size(), false);
    for (const auto& path : expected.paths) {
        bool matched = false;
        for (std::size_t index = 0; index < actual.paths.size(); ++index) {
            if (used[index]) { continue; }
            const auto& candidate = actual.paths[index];
            if (!SameAdapterId(path.sourceInfo.adapterId, candidate.sourceInfo.adapterId) ||
                path.sourceInfo.id != candidate.sourceInfo.id ||
                !SameAdapterId(path.targetInfo.adapterId, candidate.targetInfo.adapterId) ||
                path.targetInfo.id != candidate.targetInfo.id) { continue; }
            if (!ValidDisplayModeIndex(expected, path.sourceInfo.modeInfoIdx,
                    DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, path.sourceInfo.adapterId, path.sourceInfo.id) ||
                !ValidDisplayModeIndex(actual, candidate.sourceInfo.modeInfoIdx,
                    DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE, candidate.sourceInfo.adapterId, candidate.sourceInfo.id)) {
                return false;
            }
            const auto& a = expected.modes[path.sourceInfo.modeInfoIdx].sourceMode;
            const auto& b = actual.modes[candidate.sourceInfo.modeInfoIdx].sourceMode;
            const auto& ar = path.targetInfo.refreshRate;
            const auto& br = candidate.targetInfo.refreshRate;
            if (a.width != b.width || a.height != b.height || a.pixelFormat != b.pixelFormat ||
                a.position.x != b.position.x || a.position.y != b.position.y ||
                path.targetInfo.rotation != candidate.targetInfo.rotation ||
                path.targetInfo.scaling != candidate.targetInfo.scaling ||
                path.targetInfo.scanLineOrdering != candidate.targetInfo.scanLineOrdering ||
                ar.Denominator == 0 || br.Denominator == 0 ||
                static_cast<unsigned long long>(ar.Numerator) * br.Denominator !=
                    static_cast<unsigned long long>(br.Numerator) * ar.Denominator) {
                return false;
            }
            matched = true;
            used[index] = true;
            break;
        }
        if (!matched) { return false; }
    }
    return true;
}

bool ContainsDisplayViews(
    const DisplayConfiguration& expectedViews, const DisplayConfiguration& actual) {
    if (expectedViews.paths.empty()) { return true; }
    if (!RestorableDisplayConfiguration(expectedViews) || !RestorableDisplayConfiguration(actual)) {
        return false;
    }
    for (const auto& path : expectedViews.paths) {
        bool found = false;
        for (const auto& candidate : actual.paths) {
            if (!SameAdapterId(path.targetInfo.adapterId, candidate.targetInfo.adapterId) ||
                path.targetInfo.id != candidate.targetInfo.id) { continue; }
            const auto& expected = expectedViews.modes[path.sourceInfo.modeInfoIdx].sourceMode;
            const auto& observed = actual.modes[candidate.sourceInfo.modeInfoIdx].sourceMode;
            const auto& a = path.targetInfo.refreshRate;
            const auto& b = candidate.targetInfo.refreshRate;
            const auto left = static_cast<unsigned long long>(a.Numerator) * b.Denominator;
            const auto right = static_cast<unsigned long long>(b.Numerator) * a.Denominator;
            const auto delta = left > right ? left - right : right - left;
            if (expected.width != observed.width || expected.height != observed.height ||
                expected.pixelFormat != observed.pixelFormat ||
                path.targetInfo.rotation != candidate.targetInfo.rotation ||
                path.targetInfo.scanLineOrdering != candidate.targetInfo.scanLineOrdering ||
                a.Denominator == 0 || b.Denominator == 0 ||
                delta > static_cast<unsigned long long>(a.Denominator) * b.Denominator / 2) {
                return false;
            }
            found = true;
            break;
        }
        if (!found) { return false; }
    }
    return true;
}
