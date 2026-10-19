#ifndef LAN_AUDIO_PROTOCOL_H
#define LAN_AUDIO_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define LAN_AUDIO_SAMPLE_RATE 48000u
#define LAN_AUDIO_CHANNELS 2u
#define LAN_AUDIO_FRAMES_PER_PACKET 240u
#define LAN_AUDIO_HEADER_SIZE 20u
#define LAN_AUDIO_SAMPLE_COUNT (LAN_AUDIO_FRAMES_PER_PACKET * LAN_AUDIO_CHANNELS)
#define LAN_AUDIO_MAX_PACKET_SIZE (LAN_AUDIO_HEADER_SIZE + LAN_AUDIO_SAMPLE_COUNT * sizeof(int16_t))

int protocol_encode(uint8_t *destination, size_t destination_size, uint32_t sequence, const int16_t *samples);
int protocol_decode(const uint8_t *packet, size_t packet_size, uint32_t *sequence, int16_t *samples);
int protocol_self_test(void);

#endif
