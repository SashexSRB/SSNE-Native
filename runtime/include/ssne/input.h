// Controller state fed to the SI emulation (port 1 = channel 0). Written by the host (SDL / scripts).
#pragma once
#include <cstdint>

namespace ssne {

// GameCube button bits (PADStatus layout)
enum PadButton : uint16_t {
    PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008,
    PAD_Z = 0x0010, PAD_R = 0x0020, PAD_L = 0x0040,
    PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

struct PadState {
    uint16_t buttons = 0;
    uint8_t stick_x = 128, stick_y = 128, cstick_x = 128, cstick_y = 128;
    uint8_t trigger_l = 0, trigger_r = 0;
};

void set_pad(int port, const PadState& state);
PadState get_pad(int port);
// Buttons the runtime itself presses on port 1 for `ms` milliseconds (added to the host's input).
void inject_buttons(uint16_t mask, int ms);

// Rumble motor state the game last sent to a port (PADControlMotor writes it into the SI output word):
// 0 = stop, 1 = rumble, 2 = hard stop. The host turns it into gamepad rumble.
int pad_motor(int port);

// Called by the disc reader with the name of every file the game reads (drives "skip intros").
void intro_skip_file_read(const char* name);

// SI hardware (0xCC006400)
uint16_t si_read16(uint32_t off);
void si_write16(uint32_t off, uint16_t v);
void si_poll();  // called every retrace

} // namespace ssne
