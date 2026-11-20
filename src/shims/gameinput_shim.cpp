#include "scarlettx/input/gameinput_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

bool GameInputTraceEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("SCARLETTX_TRACE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kE_POINTER = static_cast<int32_t>(0x80004003u);
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kGAMEINPUT_E_READING_NOT_FOUND = static_cast<int32_t>(0x838A0002u);

constexpr uint64_t kVirtualGamepadId = 1;
constexpr uint64_t kVirtualKeyboardId = 2;

#if defined(_WIN32)
#define SCARLETTX_GI_CALL __stdcall
#else
#define SCARLETTX_GI_CALL
#endif

using GiComFn = intptr_t (SCARLETTX_GI_CALL*)(void*, uintptr_t, uintptr_t, uintptr_t);

struct ReadingEntry {
    uint64_t handle = 0;
    uint64_t device_id = 0;
    uint32_t input_kind = 0;
    uint64_t timestamp_us = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
};

struct GameInputObject {
    // First field is the COM vtable pointer so real Xbox binaries can call
    // IGameInput methods directly through the pointer returned by GameInputCreate,
    // while synthetic C tests can still treat it as an opaque uint64_t handle.
    const GiComFn* lpVtbl = nullptr;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<uint64_t> next_callback_token{1};
    std::mutex mu;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
    uint64_t next_reading_id = 1;
    std::vector<GAMEINPUT_DEVICE_INFO> devices;
    std::unordered_map<uint64_t, GAMEINPUT_RUMBLE_PARAMS> rumble;
    std::deque<ReadingEntry> history;

    uint64_t NowUs() const {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()) + 1;
    }

    void PushReading(ReadingEntry entry) {
        entry.handle = next_reading_id++;
        entry.timestamp_us = NowUs();
        history.push_back(std::move(entry));
        if (history.size() > 256) {
            history.pop_front();
        }
    }
};

struct InputState {
    std::mutex mu;
    std::unordered_map<uint64_t, std::shared_ptr<GameInputObject>> instances;
};

InputState& State() {
    static InputState s;
    return s;
}

std::shared_ptr<GameInputObject> GetInstance(uint64_t handle) {
    std::lock_guard<std::mutex> lk(State().mu);
    auto it = State().instances.find(handle);
    return it == State().instances.end() ? nullptr : it->second;
}

intptr_t SCARLETTX_GI_CALL GiQueryInterface(void* self, uintptr_t /*riid*/, uintptr_t ppv, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    auto** out = reinterpret_cast<void**>(ppv);
    if (!out) return kE_POINTER;
    if (!obj) {
        *out = nullptr;
        return kE_INVALIDARG;
    }
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *out = self;
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiAddRef(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->ref_count.fetch_add(1, std::memory_order_relaxed) + 1) : 0;
}

intptr_t SCARLETTX_GI_CALL GiRelease(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    if (!obj) return 0;
    const uint32_t rem = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (rem == 0) {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances.erase(reinterpret_cast<uint64_t>(obj));
    }
    return static_cast<intptr_t>(rem);
}

intptr_t SCARLETTX_GI_CALL GiGetCurrentTimestamp(void* self, uintptr_t, uintptr_t, uintptr_t) {
    auto* obj = static_cast<GameInputObject*>(self);
    return obj ? static_cast<intptr_t>(obj->NowUs()) : 0;
}

intptr_t SCARLETTX_GI_CALL GiGetCurrentReading(
    void* /*self*/, uintptr_t input_kind, uintptr_t /*device*/, uintptr_t ppReading) {
    if (ppReading) {
        *reinterpret_cast<void**>(ppReading) = nullptr;
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::GetCurrentReading(kind=0x"
                  << std::hex << input_kind << std::dec << ") -> GAMEINPUT_E_READING_NOT_FOUND\n";
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

// IGameInput::RegisterDeviceCallback (vtable[9] / +0x48):
//   HRESULT RegisterDeviceCallback(
//       IGameInputDevice* device, GameInputKind inputFilter,
//       GameInputDeviceStatus statusFilter, GameInputEnumerationKind enumKind,
//       void* context, GameInputDeviceCallback callbackFunc,
//       GameInputCallbackToken* callbackToken);
intptr_t SCARLETTX_GI_CALL GiRegisterDeviceCallback(
    void* self, uintptr_t /*device*/, uintptr_t input_filter, uintptr_t status_filter,
    uintptr_t enum_kind, uintptr_t /*context*/, uintptr_t /*callback_fn*/, uintptr_t out_token_ptr) {
    auto* obj = static_cast<GameInputObject*>(self);
    const uint64_t tok = obj ? obj->next_callback_token.fetch_add(1, std::memory_order_relaxed) : 1;
    if (out_token_ptr) {
        *reinterpret_cast<uint64_t*>(out_token_ptr) = tok;
    }
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::RegisterDeviceCallback(kind=0x"
                  << std::hex << input_filter << ", status=0x" << status_filter
                  << ", enum=" << std::dec << enum_kind << ") -> S_OK (tok=" << tok << ")\n";
    }
    return kS_OK;
}

intptr_t SCARLETTX_GI_CALL GiStopCallback(void* /*self*/, uintptr_t tok, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::StopCallback(tok=" << tok << ")\n";
    }
    return 0;
}

intptr_t SCARLETTX_GI_CALL GiUnregisterCallback(void* /*self*/, uintptr_t tok, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::UnregisterCallback(tok=" << tok << ") -> true\n";
    }
    return 1;
}

intptr_t SCARLETTX_GI_CALL GiSetFocusPolicy(void* /*self*/, uintptr_t policy, uintptr_t, uintptr_t) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::SetFocusPolicy(0x"
                  << std::hex << policy << std::dec << ")\n";
    }
    return 0;
}

template <int Slot>
intptr_t SCARLETTX_GI_CALL GiTracedSlot(void* /*self*/, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] IGameInput::vtable[" << Slot
                  << "](a1=0x" << std::hex << a1 << ", a2=0x" << a2 << ", a3=0x" << a3
                  << std::dec << ") -> E_NOTIMPL\n";
    }
    return kE_NOTIMPL;
}

template <size_t... Is>
constexpr std::array<GiComFn, sizeof...(Is) + 3> MakeGiVtable(std::index_sequence<Is...>) {
    return {
        &GiQueryInterface,
        &GiAddRef,
        &GiRelease,
        &GiTracedSlot<static_cast<int>(Is + 3)>...,
    };
}

std::array<GiComFn, 32> BuildGiVtable() {
    auto vtbl = MakeGiVtable(std::make_index_sequence<29>{});
    vtbl[3] = &GiGetCurrentTimestamp;
    vtbl[4] = &GiGetCurrentReading;
    vtbl[9] = reinterpret_cast<GiComFn>(&GiRegisterDeviceCallback);
    vtbl[13] = &GiStopCallback;
    vtbl[14] = &GiUnregisterCallback;
    vtbl[22] = &GiSetFocusPolicy;
    return vtbl;
}

const auto kIGameInputVtbl = BuildGiVtable();

}  // namespace

extern "C" {

int32_t GameInputCreate(uint64_t* out_game_input) {
    if (!out_game_input) return kE_INVALIDARG;
    auto obj = std::make_shared<GameInputObject>();
    obj->lpVtbl = kIGameInputVtbl.data();

    GAMEINPUT_DEVICE_INFO pad{};
    pad.DeviceId = kVirtualGamepadId;
    pad.VendorId = 0x045Eu;   // Microsoft
    pad.ProductId = 0x0B12u;  // Xbox Series X|S Wireless Controller
    pad.SupportedInput = GAMEINPUT_KIND_GAMEPAD | GAMEINPUT_KIND_CONTROLLER_BUTTON |
                         GAMEINPUT_KIND_CONTROLLER_AXIS;
    pad.SupportedRumbleMotors = 0x0Fu;  // Low, High, LeftTrigger, RightTrigger
    std::strncpy(pad.DisplayName, "ScarlettX Virtual Xbox Series Controller", sizeof(pad.DisplayName) - 1);
    obj->devices.push_back(pad);

    GAMEINPUT_DEVICE_INFO kb{};
    kb.DeviceId = kVirtualKeyboardId;
    kb.VendorId = 0x045Eu;
    kb.ProductId = 0x0001u;
    kb.SupportedInput = GAMEINPUT_KIND_KEYBOARD;
    kb.SupportedRumbleMotors = 0;
    std::strncpy(kb.DisplayName, "ScarlettX Virtual Keyboard", sizeof(kb.DisplayName) - 1);
    obj->devices.push_back(kb);

    // Seed initial neutral readings so GetCurrentReading succeeds immediately.
    ReadingEntry pad_init{};
    pad_init.device_id = kVirtualGamepadId;
    pad_init.input_kind = GAMEINPUT_KIND_GAMEPAD;
    obj->PushReading(std::move(pad_init));

    ReadingEntry kb_init{};
    kb_init.device_id = kVirtualKeyboardId;
    kb_init.input_kind = GAMEINPUT_KIND_KEYBOARD;
    obj->PushReading(std::move(kb_init));

    const uint64_t h = reinterpret_cast<uint64_t>(obj.get());
    {
        std::lock_guard<std::mutex> lk(State().mu);
        State().instances[h] = std::move(obj);
    }
    *out_game_input = h;
    if (GameInputTraceEnabled()) {
        std::cerr << "[ScarlettX][GameInput] GameInputCreate() -> S_OK (IGameInput*=0x"
                  << std::hex << h << std::dec << ")\n";
    }
    return kS_OK;
}

int32_t GameInputDestroy(uint64_t game_input) {
    std::lock_guard<std::mutex> lk(State().mu);
    return State().instances.erase(game_input) ? kS_OK : kE_HANDLE;
}

uint64_t GameInputGetCurrentTimestamp(uint64_t game_input) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    return obj->NowUs();
}

int32_t GameInputEnumerateDevices(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t* out_device_ids,
    uint32_t capacity,
    uint32_t* out_count) {
    if (!out_count) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    uint32_t count = 0;
    for (const auto& d : obj->devices) {
        if (input_kind == GAMEINPUT_KIND_UNKNOWN || (d.SupportedInput & input_kind) != 0) {
            if (out_device_ids && count < capacity) {
                out_device_ids[count] = d.DeviceId;
            }
            ++count;
        }
    }
    *out_count = count;
    return kS_OK;
}

int32_t GameInputGetDeviceInfo(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_DEVICE_INFO* out_info) {
    if (!out_info) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& d : obj->devices) {
        if (d.DeviceId == device_id) {
            *out_info = d;
            return kS_OK;
        }
    }
    return kE_INVALIDARG;
}

int32_t GameInputGetCurrentReading(
    uint64_t game_input,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetNextReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle <= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((e.input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (e.device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = e.handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

int32_t GameInputGetPreviousReading(
    uint64_t game_input,
    uint64_t reference_reading,
    uint32_t input_kind,
    uint64_t device_id,
    uint64_t* out_reading) {
    if (!out_reading) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (auto it = obj->history.rbegin(); it != obj->history.rend(); ++it) {
        if (it->handle >= reference_reading) continue;
        const bool kind_ok = (input_kind == GAMEINPUT_KIND_UNKNOWN) || ((it->input_kind & input_kind) != 0);
        const bool dev_ok = (device_id == 0) || (it->device_id == device_id);
        if (kind_ok && dev_ok) {
            *out_reading = it->handle;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint64_t GameInputGetReadingTimestamp(uint64_t game_input, uint64_t reading) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading) return e.timestamp_us;
    }
    return 0;
}

int32_t GameInputGetGamepadState(
    uint64_t game_input,
    uint64_t reading,
    GAMEINPUT_GAMEPAD_STATE* out_state) {
    if (!out_state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_GAMEPAD) {
            *out_state = e.gamepad;
            return kS_OK;
        }
    }
    return kGAMEINPUT_E_READING_NOT_FOUND;
}

uint32_t GameInputGetKeyState(
    uint64_t game_input,
    uint64_t reading,
    uint32_t max_keys,
    GAMEINPUT_KEY_STATE* out_keys) {
    auto obj = GetInstance(game_input);
    if (!obj) return 0;
    std::lock_guard<std::mutex> lk(obj->mu);
    for (const auto& e : obj->history) {
        if (e.handle == reading && e.input_kind == GAMEINPUT_KIND_KEYBOARD) {
            const uint32_t n = std::min<uint32_t>(max_keys, static_cast<uint32_t>(e.keys.size()));
            if (out_keys && n > 0) {
                std::memcpy(out_keys, e.keys.data(), n * sizeof(GAMEINPUT_KEY_STATE));
            }
            return n;
        }
    }
    return 0;
}

int32_t GameInputSetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_RUMBLE_PARAMS* params) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    std::lock_guard<std::mutex> lk(obj->mu);
    GAMEINPUT_RUMBLE_PARAMS p{};
    if (params) {
        p.LowFrequency = std::clamp(params->LowFrequency, 0.0f, 1.0f);
        p.HighFrequency = std::clamp(params->HighFrequency, 0.0f, 1.0f);
        p.LeftTrigger = std::clamp(params->LeftTrigger, 0.0f, 1.0f);
        p.RightTrigger = std::clamp(params->RightTrigger, 0.0f, 1.0f);
    }
    obj->rumble[device_id] = p;
    return kS_OK;
}

int32_t GameInputGetRumbleState(
    uint64_t game_input,
    uint64_t device_id,
    GAMEINPUT_RUMBLE_PARAMS* out_params) {
    if (!out_params) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;
    std::lock_guard<std::mutex> lk(obj->mu);
    auto it = obj->rumble.find(device_id);
    if (it == obj->rumble.end()) {
        *out_params = {};
    } else {
        *out_params = it->second;
    }
    return kS_OK;
}

int32_t GameInputInjectGamepadState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_GAMEPAD_STATE* state) {
    if (!state) return kE_INVALIDARG;
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualGamepadId : device_id;
    entry.input_kind = GAMEINPUT_KIND_GAMEPAD;
    entry.gamepad = *state;
    entry.gamepad.LeftTrigger = std::clamp(entry.gamepad.LeftTrigger, 0.0f, 1.0f);
    entry.gamepad.RightTrigger = std::clamp(entry.gamepad.RightTrigger, 0.0f, 1.0f);
    entry.gamepad.LeftThumbstickX = std::clamp(entry.gamepad.LeftThumbstickX, -1.0f, 1.0f);
    entry.gamepad.LeftThumbstickY = std::clamp(entry.gamepad.LeftThumbstickY, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickX = std::clamp(entry.gamepad.RightThumbstickX, -1.0f, 1.0f);
    entry.gamepad.RightThumbstickY = std::clamp(entry.gamepad.RightThumbstickY, -1.0f, 1.0f);

    std::lock_guard<std::mutex> lk(obj->mu);
    obj->PushReading(std::move(entry));
    return kS_OK;
}

int32_t GameInputInjectKeyState(
    uint64_t game_input,
    uint64_t device_id,
    const GAMEINPUT_KEY_STATE* keys,
    uint32_t key_count) {
    auto obj = GetInstance(game_input);
    if (!obj) return kE_HANDLE;

    ReadingEntry entry{};
    entry.device_id = device_id == 0 ? kVirtualKeyboardId : device_id;
    entry.input_kind = GAMEINPUT_KIND_KEYBOARD;
    if (keys && key_count > 0) {
        entry.keys.assign(keys, keys + key_count);
    }

    std::lock_guard<std::mutex> lk(obj->mu);
    obj->PushReading(std::move(entry));
    return kS_OK;
}

}  // extern "C"
