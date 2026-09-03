/*
 * No-op audio stub for the photo-frame MVP.
 *
 * local_photo_slideshow.cpp calls audio::play_tone_from_midi() from its
 * (unused, in one-shot mode) button-handling path, so the symbol must exist
 * to link. The real M5PaperColor-UserDemo implementation drives the ES8311
 * speaker codec; sound is out of scope for this photo frame, so these are
 * intentionally empty.
 */
#include "audio.h"

namespace audio {

void play_tone(int frequency, double duration_sec) {}

void play_melody(const std::vector<int>& midi_list, double duration_sec) {}

void play_tone_from_midi(int midi, double duration_sec) {}

void play_random_tone(int semitone_shift, double duration_sec) {}

}  // namespace audio
