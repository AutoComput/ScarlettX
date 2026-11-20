#include "scarlettx/input/gameinput_abi.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr int32_t kS_OK = 0;
constexpr int32_t kE_INVALIDARG = static_cast<int32_t>(0x80070057u);
constexpr int32_t kE_HANDLE = static_cast<int32_t>(0x80070006u);
constexpr int32_t kGAMEINPUT_E_READING_NOT_FOUND = static_cast<int32_t>(0x838A0002u);

constexpr uint64_t kVirtualGamepadId = 1;
constexpr uint64_t kVirtualKeyboardId = 2;

struct ReadingEntry {
    uint64_t handle = 0;
    uint64_t device_id = 0;
    uint32_t input_kind = 0;
    uint64_t timestamp_us = 0;
    GAMEINPUT_GAMEPAD_STATE gamepad{};
    std::vector<GAMEINPUT_KEY_STATE> keys;
};

struct GameInputObject {
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
    uint64_t next_handle = 1;
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

}  // namespace

extern "C" {

int32_t GameInputCreate(uint64_t* out_game_input) {
    if (!out_game_input) return kE_INVALIDARG;
    auto obj = std::make_shared<GameInputObject>();

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

    std::lock_guard<std::mutex> lk(State().mu);
    const uint64_t h = State().next_handle++;
    State().instances[h] = std::move(obj);
    *out_game_input = h;
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
