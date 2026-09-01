#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void audio_bsp_init(void);
int volume_adjustment(uint8_t vol);

/* Recording support: register a callback to receive mic PCM samples while recording */
typedef void (*audio_record_cb_t)(const int16_t *samples, size_t num_samples);
void audio_set_record_callback(audio_record_cb_t cb);
void audio_start_recording_ms(uint32_t duration_ms);
void audio_stop_recording(void);
bool audio_is_recording(void);

#ifdef __cplusplus
}
#endif

#endif
