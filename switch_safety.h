#pragma once

#include <windows.h>

#include <utility>

// Repeated launches must not interleave topology/color/DPI/power mutations.
class SwitchLock {
public:
    explicit SwitchLock(const wchar_t* name = L"Local\\ResSwitcher.DisplaySwitch") {
        handle_ = CreateMutexW(nullptr, FALSE, name);
        if (handle_) {
            const DWORD status = WaitForSingleObject(handle_, 0);
            owned_ = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
        }
    }
    SwitchLock(const SwitchLock&) = delete;
    SwitchLock& operator=(const SwitchLock&) = delete;
    ~SwitchLock() {
        Release();
        if (handle_) { CloseHandle(handle_); }
    }
    bool Acquired() const { return owned_; }
    void Release() {
        if (owned_ && ReleaseMutex(handle_)) { owned_ = false; }
    }

private:
    HANDLE handle_ = nullptr;
    bool owned_ = false;
};

template <typename Action>
class RollbackAction {
public:
    explicit RollbackAction(Action action) : action_(std::move(action)) {}
    RollbackAction(const RollbackAction&) = delete;
    RollbackAction& operator=(const RollbackAction&) = delete;
    ~RollbackAction() noexcept { Run(); }
    void Arm() { armed_ = true; }
    void Commit() { armed_ = false; }
    void Run() noexcept {
        if (armed_) {
            armed_ = false;
            try { action_(); } catch (...) { OutputDebugStringW(L"Res Switcher rollback failed.\n"); }
        }
    }

private:
    Action action_;
    bool armed_ = false;
};
