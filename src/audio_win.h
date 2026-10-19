#ifndef LAN_AUDIO_WIN_H
#define LAN_AUDIO_WIN_H

#include <stdint.h>

typedef struct AudioCapture AudioCapture;
typedef struct AudioRender AudioRender;
typedef void (*AudioFrameCallback)(const int16_t *stereo_samples, uint32_t frame_count, void *context);
typedef int (*AudioSampleReader)(int16_t *left, int16_t *right, void *context);

AudioCapture *audio_capture_open(int loopback, char *error, unsigned int error_size);
int audio_capture_read(AudioCapture *capture, AudioFrameCallback callback, void *context, char *error, unsigned int error_size);
void audio_capture_close(AudioCapture *capture);

AudioRender *audio_render_open(char *error, unsigned int error_size);
int audio_render_start(AudioRender *render, char *error, unsigned int error_size);
int audio_render_write(AudioRender *render, AudioSampleReader reader, void *context, char *error, unsigned int error_size);
void audio_render_close(AudioRender *render);

#endif
