// Audio: AX DSP microcode HLE (voice decoding + mixing done natively) and the AI DMA output FIFO.
#pragma once
#include <cstdint>

namespace ssne {

// Called by the DSP mailbox emulation when the AX library sends a command list (0xBABExxxx + address).
void ax_process_cmdlist(uint32_t addr, uint16_t size_words);

// AI DMA output. The emulated AI pushes interleaved big-endian R,L s16 frames here; the host pulls
// interleaved native L,R s16 frames. Missing frames are filled with silence.
void audio_push_ai(const uint8_t* be_rl_frames, uint32_t frames, uint32_t rate);
uint32_t audio_pull(int16_t* lr_out, uint32_t frames);  // returns frames that were real audio
uint32_t audio_rate();                                  // current AI DMA rate (32000 or 48000)
uint32_t audio_buffered();                              // frames waiting in the FIFO

// Called when the host has no audio device (headless): keep the FIFO from growing without bound.
void audio_set_sink_active(bool active);

// Voice resampling quality. Original (false): the DSP's own methods (linear interpolation, 4-tap polyphase).
// High quality (true, default): voices that change rate use a 64-tap band-limited (windowed-sinc) filter instead,
// which removes the aliasing and dulled treble of linear interpolation (the game's music is 44.1 kHz resampled
// to the DSP's 32 kHz with linear interpolation).
void audio_set_hq(bool hq);
// Music & video sound vs sound effects volume (0-100 %, on top of the master volume).
void audio_set_mix(int music_pct, int sfx_pct);
bool audio_hq();

struct AudioStats { uint64_t cmdlists, voices_processed, ai_frames, underruns, overruns; uint32_t active_voices; };
AudioStats audio_stats();

} // namespace ssne
