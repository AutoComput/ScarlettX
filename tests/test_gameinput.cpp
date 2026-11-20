#include <cmath>
#include <iostream>
#include <string>

#include "scarlettx/input/gameinput_abi.h"

#define SX_CHECK(cond)                                                          \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::cerr << "[test_gameinput] FAILED: " #cond                      \
                      << " at line " << __LINE__ << "\n";                       \
            return 1;                                                           \
        }                                                                       \
    } while (0)

#define SX_CHECK_EQ(a, b)                                                       \
    do {                                                                        \
        const auto _a = (a);                                                    \
        const auto _b = (b);                                                    \
        if (!(_a == _b)) {                                                      \
            std::cerr << "[test_gameinput] FAILED: " #a " == " #b               \
                      << " (" << _a << " != " << _b << ") at line " << __LINE__ \
                      << "\n";                                                  \
            return 1;                                                           \
        }                                                                       \
    } while (0)

int main() {
    uint64_t gi = 0;
    SX_CHECK_EQ(GameInputCreate(&gi), 0);
    SX_CHECK(gi != 0);
    SX_CHECK(GameInputGetCurrentTimestamp(gi) > 0);

    // 1. Enumerate virtual Scarlett controller
    uint64_t dev_ids[4] = {};
    uint32_t dev_count = 0;
    SX_CHECK_EQ(GameInputEnumerateDevices(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids, 4, &dev_count), 0);
    SX_CHECK_EQ(dev_count, 1u);

    GAMEINPUT_DEVICE_INFO info{};
    SX_CHECK_EQ(GameInputGetDeviceInfo(gi, dev_ids[0], &info), 0);
    SX_CHECK_EQ(info.VendorId, 0x045Eu);
    SX_CHECK_EQ(info.ProductId, 0x0B12u);
    SX_CHECK_EQ(info.SupportedRumbleMotors, 0x0Fu);
    SX_CHECK(std::string(info.DisplayName).find("Xbox Series") != std::string::npos);

    // 2. Initial neutral reading
    uint64_t r0 = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r0), 0);
    GAMEINPUT_GAMEPAD_STATE s0{};
    SX_CHECK_EQ(GameInputGetGamepadState(gi, r0, &s0), 0);
    SX_CHECK_EQ(s0.Buttons, 0u);

    // 3. Inject two consecutive gamepad states and traverse Next/Previous history
    GAMEINPUT_GAMEPAD_STATE inj1{};
    inj1.Buttons = GAMEINPUT_GAMEPAD_A | GAMEINPUT_GAMEPAD_RIGHT_SHOULDER;
    inj1.RightTrigger = 0.75f;
    inj1.LeftThumbstickX = 0.5f;
    inj1.LeftThumbstickY = -1.5f;  // should clamp to -1.0f
    SX_CHECK_EQ(GameInputInjectGamepadState(gi, dev_ids[0], &inj1), 0);

    GAMEINPUT_GAMEPAD_STATE inj2{};
    inj2.Buttons = GAMEINPUT_GAMEPAD_X;
    inj2.LeftTrigger = 1.0f;
    SX_CHECK_EQ(GameInputInjectGamepadState(gi, dev_ids[0], &inj2), 0);

    uint64_t r1 = 0;
    SX_CHECK_EQ(GameInputGetNextReading(gi, r0, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r1), 0);
    GAMEINPUT_GAMEPAD_STATE s1{};
    SX_CHECK_EQ(GameInputGetGamepadState(gi, r1, &s1), 0);
    SX_CHECK_EQ(s1.Buttons, static_cast<uint32_t>(GAMEINPUT_GAMEPAD_A | GAMEINPUT_GAMEPAD_RIGHT_SHOULDER));
    SX_CHECK(std::fabs(s1.RightTrigger - 0.75f) < 1e-5f);
    SX_CHECK(std::fabs(s1.LeftThumbstickY - (-1.0f)) < 1e-5f);

    uint64_t r2 = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &r2), 0);
    SX_CHECK(r2 > r1);
    uint64_t prev = 0;
    SX_CHECK_EQ(GameInputGetPreviousReading(gi, r2, GAMEINPUT_KIND_GAMEPAD, dev_ids[0], &prev), 0);
    SX_CHECK_EQ(prev, r1);
    SX_CHECK(GameInputGetReadingTimestamp(gi, r2) >= GameInputGetReadingTimestamp(gi, r1));

    // 4. Impulse-trigger + main rumble motors
    GAMEINPUT_RUMBLE_PARAMS rumble{0.6f, 0.4f, 0.8f, 0.25f};
    SX_CHECK_EQ(GameInputSetRumbleState(gi, dev_ids[0], &rumble), 0);
    GAMEINPUT_RUMBLE_PARAMS got_rumble{};
    SX_CHECK_EQ(GameInputGetRumbleState(gi, dev_ids[0], &got_rumble), 0);
    SX_CHECK(std::fabs(got_rumble.LowFrequency - 0.6f) < 1e-5f);
    SX_CHECK(std::fabs(got_rumble.LeftTrigger - 0.8f) < 1e-5f);

    // 5. Keyboard injection & reading
    GAMEINPUT_KEY_STATE keys[2]{};
    keys[0].VirtualKey = 'W';
    keys[0].CodePoint = 'w';
    keys[1].VirtualKey = ' ';
    keys[1].CodePoint = ' ';
    SX_CHECK_EQ(GameInputInjectKeyState(gi, 0, keys, 2), 0);

    uint64_t rkb = 0;
    SX_CHECK_EQ(GameInputGetCurrentReading(gi, GAMEINPUT_KIND_KEYBOARD, 0, &rkb), 0);
    GAMEINPUT_KEY_STATE read_keys[4]{};
    const uint32_t nkeys = GameInputGetKeyState(gi, rkb, 4, read_keys);
    SX_CHECK_EQ(nkeys, 2u);
    SX_CHECK_EQ(read_keys[0].VirtualKey, 'W');

    SX_CHECK_EQ(GameInputDestroy(gi), 0);
    std::cout << "[test_gameinput] All GameInput HLE checks passed.\n";
    return 0;
}
