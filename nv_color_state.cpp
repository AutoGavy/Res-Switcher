#include "nv_color_state.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

namespace {

// Minimal ABI-compatible declarations from NVIDIA's MIT-licensed NVAPI SDK.
// The implementation itself is supplied by the installed NVIDIA display driver.
using NvStatus = int;

constexpr NvStatus kNvOk = 0;
constexpr std::uint8_t kColorCommandGet = 1;
constexpr std::uint8_t kColorCommandSet = 2;
constexpr std::uint8_t kColorCommandIsSupported = 3;
constexpr std::int32_t kColorSelectionPolicyUser = 0;

constexpr std::uint32_t kNvApiInitializeId = 0x0150E828;
constexpr std::uint32_t kNvApiUnloadId = 0xD22BDD7E;
constexpr std::uint32_t kNvApiGetErrorMessageId = 0x6C2D048C;
constexpr std::uint32_t kNvApiGetPrimaryDisplayId = 0x1E9D8A31;
constexpr std::uint32_t kNvApiColorControlId = 0x92F9D80D;

template <typename T>
constexpr std::uint32_t MakeVersion(const std::uint32_t version) {
    return static_cast<std::uint32_t>(sizeof(T)) | (version << 16);
}

struct NvColorDataV3 {
    std::uint32_t version;
    std::uint16_t size;
    std::uint8_t command;
    struct {
        std::uint8_t colorFormat;
        std::uint8_t colorimetry;
        std::uint8_t dynamicRange;
        std::int32_t bpc;
    } data;
};

struct NvColorDataV4 {
    std::uint32_t version;
    std::uint16_t size;
    std::uint8_t command;
    struct {
        std::uint8_t colorFormat;
        std::uint8_t colorimetry;
        std::uint8_t dynamicRange;
        std::int32_t bpc;
        std::int32_t colorSelectionPolicy;
    } data;
};

struct NvColorDataV5 {
    std::uint32_t version;
    std::uint16_t size;
    std::uint8_t command;
    struct {
        std::uint8_t colorFormat;
        std::uint8_t colorimetry;
        std::uint8_t dynamicRange;
        std::int32_t bpc;
        std::int32_t colorSelectionPolicy;
        std::int32_t desktopDepth;
    } data;
};

static_assert(sizeof(NvColorDataV3) == 16);
static_assert(sizeof(NvColorDataV4) == 20);
static_assert(sizeof(NvColorDataV5) == 24);

using QueryInterfaceFn = void*(__cdecl*)(std::uint32_t);
using InitializeFn = NvStatus(__cdecl*)();
using UnloadFn = NvStatus(__cdecl*)();
using GetErrorMessageFn = NvStatus(__cdecl*)(NvStatus, char*);
using GetPrimaryDisplayIdFn = NvStatus(__cdecl*)(std::uint32_t*);
using ColorControlFn = NvStatus(__cdecl*)(std::uint32_t, void*);

std::wstring ToWide(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), required);
    result.pop_back();
    return result;
}

class NvApiSession {
public:
    NvApiSession() = default;
    NvApiSession(const NvApiSession&) = delete;
    NvApiSession& operator=(const NvApiSession&) = delete;

    ~NvApiSession() {
        if (initialized_ && unload_ != nullptr) {
            unload_();
        }
        if (module_ != nullptr) {
            FreeLibrary(module_);
        }
    }

    bool Open(std::wstring& error) {
#if defined(_WIN64)
        module_ = LoadLibraryW(L"nvapi64.dll");
#else
        module_ = LoadLibraryW(L"nvapi.dll");
#endif
        if (module_ == nullptr) {
            error = L"the NVIDIA NVAPI driver library is unavailable";
            return false;
        }

        const auto queryInterface = reinterpret_cast<QueryInterfaceFn>(
            GetProcAddress(module_, "nvapi_QueryInterface"));
        if (queryInterface == nullptr) {
            error = L"the NVIDIA driver does not export nvapi_QueryInterface";
            return false;
        }

        initialize_ = Resolve<InitializeFn>(queryInterface, kNvApiInitializeId);
        unload_ = Resolve<UnloadFn>(queryInterface, kNvApiUnloadId);
        getErrorMessage_ = Resolve<GetErrorMessageFn>(
            queryInterface,
            kNvApiGetErrorMessageId);
        getPrimaryDisplayId_ = Resolve<GetPrimaryDisplayIdFn>(
            queryInterface,
            kNvApiGetPrimaryDisplayId);
        colorControl_ = Resolve<ColorControlFn>(
            queryInterface,
            kNvApiColorControlId);

        if (initialize_ == nullptr || getPrimaryDisplayId_ == nullptr ||
            colorControl_ == nullptr) {
            error = L"the NVIDIA driver does not provide the required color-control APIs";
            return false;
        }

        const NvStatus status = initialize_();
        if (status != kNvOk) {
            error = L"NVAPI initialization failed: " + DescribeStatus(status);
            return false;
        }

        initialized_ = true;
        return true;
    }

    NvStatus Execute(void* colorData) const {
        std::uint32_t displayId = 0;
        const NvStatus displayStatus = getPrimaryDisplayId_(&displayId);
        if (displayStatus != kNvOk) {
            return displayStatus;
        }
        return colorControl_(displayId, colorData);
    }

    std::wstring DescribeStatus(const NvStatus status) const {
        std::array<char, 64> message{};
        if (getErrorMessage_ != nullptr &&
            getErrorMessage_(status, message.data()) == kNvOk &&
            message[0] != '\0') {
            return ToWide(message.data()) + L" (" + std::to_wstring(status) + L")";
        }
        return L"NVAPI status " + std::to_wstring(status);
    }

private:
    template <typename T>
    static T Resolve(
        const QueryInterfaceFn queryInterface,
        const std::uint32_t id) {
        return reinterpret_cast<T>(queryInterface(id));
    }

    HMODULE module_ = nullptr;
    InitializeFn initialize_ = nullptr;
    UnloadFn unload_ = nullptr;
    GetErrorMessageFn getErrorMessage_ = nullptr;
    GetPrimaryDisplayIdFn getPrimaryDisplayId_ = nullptr;
    ColorControlFn colorControl_ = nullptr;
    bool initialized_ = false;
};

template <typename T>
T MakeColorData(
    const std::uint32_t version,
    const std::uint8_t command,
    const NvidiaColorState* state = nullptr) {
    T data{};
    data.version = MakeVersion<T>(version);
    data.size = static_cast<std::uint16_t>(sizeof(T));
    data.command = command;

    if (state != nullptr) {
        data.data.colorFormat = state->colorFormat;
        data.data.colorimetry = state->colorimetry;
        data.data.dynamicRange = state->dynamicRange;
        data.data.bpc = state->bpc;
    }
    return data;
}

NvColorDataV4 MakeColorDataV4(
    const std::uint8_t command,
    const NvidiaColorState* state = nullptr) {
    auto data = MakeColorData<NvColorDataV4>(4, command, state);
    if (state != nullptr) {
        data.data.colorSelectionPolicy = kColorSelectionPolicyUser;
    }
    return data;
}

NvColorDataV5 MakeColorDataV5(
    const std::uint8_t command,
    const NvidiaColorState* state = nullptr) {
    auto data = MakeColorData<NvColorDataV5>(5, command, state);
    if (state != nullptr) {
        data.data.colorSelectionPolicy = kColorSelectionPolicyUser;
        data.data.desktopDepth = state->desktopDepth;
    }
    return data;
}

NvidiaColorState ToState(const NvColorDataV3& data) {
    return NvidiaColorState{
        data.data.colorFormat,
        data.data.colorimetry,
        data.data.dynamicRange,
        data.data.bpc,
        0,
        3,
        false};
}

NvidiaColorState ToState(const NvColorDataV4& data) {
    return NvidiaColorState{
        data.data.colorFormat,
        data.data.colorimetry,
        data.data.dynamicRange,
        data.data.bpc,
        0,
        4,
        false};
}

NvidiaColorState ToState(const NvColorDataV5& data) {
    return NvidiaColorState{
        data.data.colorFormat,
        data.data.colorimetry,
        data.data.dynamicRange,
        data.data.bpc,
        data.data.desktopDepth,
        5,
        true};
}

bool ReadOnce(
    const NvApiSession& session,
    NvidiaColorState& state,
    NvStatus& lastStatus) {
    auto version5 = MakeColorDataV5(kColorCommandGet);
    lastStatus = session.Execute(&version5);
    if (lastStatus == kNvOk) {
        state = ToState(version5);
        return true;
    }

    auto version4 = MakeColorDataV4(kColorCommandGet);
    lastStatus = session.Execute(&version4);
    if (lastStatus == kNvOk) {
        state = ToState(version4);
        return true;
    }

    auto version3 = MakeColorData<NvColorDataV3>(3, kColorCommandGet);
    lastStatus = session.Execute(&version3);
    if (lastStatus == kNvOk) {
        state = ToState(version3);
        return true;
    }

    return false;
}

NvStatus ExecuteStateCommand(
    const NvApiSession& session,
    const NvidiaColorState& state,
    const std::uint8_t command) {
    if (state.apiVersion >= 5 && state.hasDesktopDepth) {
        auto data = MakeColorDataV5(command, &state);
        return session.Execute(&data);
    }
    if (state.apiVersion >= 4) {
        auto data = MakeColorDataV4(command, &state);
        return session.Execute(&data);
    }

    auto data = MakeColorData<NvColorDataV3>(3, command, &state);
    return session.Execute(&data);
}

template <typename Action>
bool RunWithRetry(
    const wchar_t* operation,
    const unsigned int attempts,
    Action action,
    std::wstring& error) {
    NvApiSession session;
    if (!session.Open(error)) {
        return false;
    }

    NvStatus lastStatus = kNvOk;
    const unsigned int count = attempts == 0 ? 1 : attempts;
    for (unsigned int attempt = 0; attempt < count; ++attempt) {
        if (action(session, lastStatus)) {
            return true;
        }
        if (attempt + 1 < count) {
            Sleep(100);
        }
    }

    error = std::wstring{operation} + L" failed: " + session.DescribeStatus(lastStatus);
    return false;
}

std::wstring ColorFormatName(const std::uint8_t value) {
    switch (value) {
    case 0:
        return L"RGB";
    case 1:
        return L"YCbCr 4:2:2";
    case 2:
        return L"YCbCr 4:4:4";
    case 3:
        return L"YCbCr 4:2:0";
    case 0xFE:
        return L"Default";
    case 0xFF:
        return L"Auto";
    default:
        return L"Format " + std::to_wstring(value);
    }
}

std::wstring ColorimetryName(const std::uint8_t value) {
    switch (value) {
    case 0:
        return L"RGB";
    case 1:
        return L"YCC 601";
    case 2:
        return L"YCC 709";
    case 8:
        return L"BT.2020 RGB";
    case 9:
        return L"BT.2020 YCC";
    case 10:
        return L"BT.2020 cYCC";
    case 0xFE:
        return L"Default";
    case 0xFF:
        return L"Auto";
    default:
        return L"Colorimetry " + std::to_wstring(value);
    }
}

std::wstring DynamicRangeName(const std::uint8_t value) {
    switch (value) {
    case 0:
        return L"Full";
    case 1:
        return L"Limited";
    case 0xFF:
        return L"Auto";
    default:
        return L"Range " + std::to_wstring(value);
    }
}

std::wstring BpcName(const std::int32_t value) {
    switch (value) {
    case 0:
        return L"Default bpc";
    case 1:
        return L"6 bpc";
    case 2:
        return L"8 bpc";
    case 3:
        return L"10 bpc";
    case 4:
        return L"12 bpc";
    case 5:
        return L"16 bpc";
    default:
        return L"BPC " + std::to_wstring(value);
    }
}

} // namespace

bool NvidiaColorControl::ReadPrimary(
    NvidiaColorState& state,
    std::wstring& error,
    const unsigned int attempts) {
    return RunWithRetry(
        L"reading NVIDIA color settings",
        attempts,
        [&state](const NvApiSession& session, NvStatus& status) {
            return ReadOnce(session, state, status);
        },
        error);
}

bool NvidiaColorControl::IsSupportedOnPrimary(
    const NvidiaColorState& state,
    std::wstring& error,
    const unsigned int attempts) {
    return RunWithRetry(
        L"checking NVIDIA color settings",
        attempts,
        [&state](const NvApiSession& session, NvStatus& status) {
            status = ExecuteStateCommand(
                session,
                state,
                kColorCommandIsSupported);
            return status == kNvOk;
        },
        error);
}

bool NvidiaColorControl::ApplyToPrimary(
    const NvidiaColorState& state,
    std::wstring& error,
    const unsigned int attempts) {
    return RunWithRetry(
        L"restoring NVIDIA color settings",
        attempts,
        [&state](const NvApiSession& session, NvStatus& status) {
            status = ExecuteStateCommand(session, state, kColorCommandSet);
            return status == kNvOk;
        },
        error);
}

bool NvidiaColorControl::Equivalent(
    const NvidiaColorState& left,
    const NvidiaColorState& right) {
    return left.colorFormat == right.colorFormat &&
           left.colorimetry == right.colorimetry &&
           left.dynamicRange == right.dynamicRange &&
           left.bpc == right.bpc &&
           (!left.hasDesktopDepth || !right.hasDesktopDepth ||
            left.desktopDepth == right.desktopDepth);
}

std::wstring NvidiaColorControl::Describe(const NvidiaColorState& state) {
    return ColorFormatName(state.colorFormat) + L", " +
           ColorimetryName(state.colorimetry) + L", " +
           DynamicRangeName(state.dynamicRange) + L", " +
           BpcName(state.bpc);
}
