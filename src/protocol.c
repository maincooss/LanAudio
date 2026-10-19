#include "protocol.h"

#include <string.h>

static void write_u16_be(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value >> 8);
    destination[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *source)
{
    return (uint16_t)(((uint16_t)source[0] << 8) | source[1]);
}

static uint32_t read_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24)
        | ((uint32_t)source[1] << 16)
        | ((uint32_t)source[2] << 8)
        | (uint32_t)source[3];
}

int protocol_encode(uint8_t *destination, size_t destination_size, uint32_t sequence, const int16_t *samples)
{
    size_t index;

    if (destination == NULL || samples == NULL || destination_size < LAN_AUDIO_MAX_PACKET_SIZE)
    {
        return 0;
    }

    memcpy(destination, "LNA1", 4);
    destination[4] = 1;
    destination[5] = LAN_AUDIO_CHANNELS;
    write_u16_be(destination + 6, LAN_AUDIO_FRAMES_PER_PACKET);
    write_u32_be(destination + 8, sequence);
    write_u32_be(destination + 12, LAN_AUDIO_SAMPLE_RATE);
    memset(destination + 16, 0, 4);

    for (index = 0; index < LAN_AUDIO_SAMPLE_COUNT; ++index)
    {
        write_u16_be(destination + LAN_AUDIO_HEADER_SIZE + index * 2, (uint16_t)samples[index]);
    }

    return (int)LAN_AUDIO_MAX_PACKET_SIZE;
}

int protocol_decode(const uint8_t *packet, size_t packet_size, uint32_t *sequence, int16_t *samples)
{
    size_t index;

    if (packet == NULL || sequence == NULL || samples == NULL || packet_size != LAN_AUDIO_MAX_PACKET_SIZE)
    {
        return 0;
    }

    if (memcmp(packet, "LNA1", 4) != 0
        || packet[4] != 1
        || packet[5] != LAN_AUDIO_CHANNELS
        || read_u16_be(packet + 6) != LAN_AUDIO_FRAMES_PER_PACKET
        || read_u32_be(packet + 12) != LAN_AUDIO_SAMPLE_RATE)
    {
        return 0;
    }

    *sequence = read_u32_be(packet + 8);
    for (index = 0; index < LAN_AUDIO_SAMPLE_COUNT; ++index)
    {
        samples[index] = (int16_t)read_u16_be(packet + LAN_AUDIO_HEADER_SIZE + index * 2);
    }

    return 1;
}

int protocol_self_test(void)
{
    uint8_t packet[LAN_AUDIO_MAX_PACKET_SIZE];
    int16_t input[LAN_AUDIO_SAMPLE_COUNT];
    int16_t output[LAN_AUDIO_SAMPLE_COUNT];
    uint32_t sequence = 0;
    size_t index;

    for (index = 0; index < LAN_AUDIO_SAMPLE_COUNT; ++index)
    {
        input[index] = (int16_t)((index * 997u) ^ 0xA55Au);
    }
    input[0] = INT16_MIN;
    input[1] = INT16_MAX;

    if (protocol_encode(packet, sizeof(packet), 0xFEDCBA98u, input) != (int)sizeof(packet)
        || !protocol_decode(packet, sizeof(packet), &sequence, output)
        || sequence != 0xFEDCBA98u
        || memcmp(input, output, sizeof(input)) != 0)
    {
        return 0;
    }

    return !protocol_decode(packet, sizeof(packet) - 1, &sequence, output)
        && (packet[4] = 2, !protocol_decode(packet, sizeof(packet), &sequence, output));
}
