#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include "audio_win.h"
#include "protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CLIENT_BUFFER_HNS 100000
#define CAPTURE_OUTPUT_FRAMES 512

static const CLSID LAN_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID LAN_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID LAN_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID LAN_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
static const IID LAN_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

typedef enum AudioSampleFormat
{
    AUDIO_SAMPLE_PCM,
    AUDIO_SAMPLE_FLOAT
} AudioSampleFormat;

struct AudioCapture
{
    IMMDevice *device;
    IAudioClient *client;
    IAudioCaptureClient *capture_client;
    WAVEFORMATEX *format;
    AudioSampleFormat sample_format;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t bytes_per_sample;
    double resample_position;
    float previous_left;
    float previous_right;
    int has_previous;
};

struct AudioRender
{
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render_client;
    WAVEFORMATEX *format;
    AudioSampleFormat sample_format;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t bytes_per_sample;
    int started;
    int have_current;
    int have_next;
    int16_t current_left;
    int16_t current_right;
    int16_t next_left;
    int16_t next_right;
    double resample_position;
};

static void set_error(char *error, unsigned int error_size, const char *operation, HRESULT result)
{
    if (error != NULL && error_size > 0)
    {
        (void)snprintf(error, error_size, "%s failed (HRESULT 0x%08lX)", operation, (unsigned long)result);
    }
}

static int describe_format(const WAVEFORMATEX *format, AudioSampleFormat *sample_format, uint16_t *bytes_per_sample)
{
    int is_float = 0;

    if (format == NULL || format->nSamplesPerSec == 0 || format->nChannels == 0 || format->nBlockAlign == 0)
    {
        return 0;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        is_float = 1;
    }
    else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE *extended = (const WAVEFORMATEXTENSIBLE *)format;
        if (extended->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT)
        {
            is_float = 1;
        }
        else if (extended->SubFormat.Data1 != WAVE_FORMAT_PCM)
        {
            return 0;
        }
    }
    else if (format->wFormatTag != WAVE_FORMAT_PCM)
    {
        return 0;
    }

    if (is_float)
    {
        if (format->wBitsPerSample != 32)
        {
            return 0;
        }
    }
    else if (format->wBitsPerSample != 8 && format->wBitsPerSample != 16
        && format->wBitsPerSample != 24 && format->wBitsPerSample != 32)
    {
        return 0;
    }

    *sample_format = is_float ? AUDIO_SAMPLE_FLOAT : AUDIO_SAMPLE_PCM;
    *bytes_per_sample = (uint16_t)(format->wBitsPerSample / 8);
    return format->nBlockAlign >= format->nChannels * *bytes_per_sample;
}

static float decode_sample(const uint8_t *sample, AudioSampleFormat format, uint16_t bits_per_sample)
{
    if (format == AUDIO_SAMPLE_FLOAT)
    {
        float value;
        memcpy(&value, sample, sizeof(value));
        return isfinite(value) ? value : 0.0f;
    }

    switch (bits_per_sample)
    {
    case 8:
        return ((float)sample[0] - 128.0f) / 128.0f;
    case 16:
    {
        int16_t value;
        memcpy(&value, sample, sizeof(value));
        return (float)value / 32768.0f;
    }
    case 24:
    {
        int32_t value = (int32_t)sample[0] | ((int32_t)sample[1] << 8) | ((int32_t)sample[2] << 16);
        if ((value & 0x800000) != 0)
        {
            value |= (int32_t)0xFF000000u;
        }
        return (float)value / 8388608.0f;
    }
    case 32:
    {
        int32_t value;
        memcpy(&value, sample, sizeof(value));
        return (float)((double)value / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

static void encode_sample(uint8_t *destination, AudioSampleFormat format, uint16_t bits_per_sample, float value)
{
    if (!isfinite(value))
    {
        value = 0.0f;
    }
    if (value > 1.0f)
    {
        value = 1.0f;
    }
    else if (value < -1.0f)
    {
        value = -1.0f;
    }

    if (format == AUDIO_SAMPLE_FLOAT)
    {
        memcpy(destination, &value, sizeof(value));
        return;
    }

    switch (bits_per_sample)
    {
    case 8:
        destination[0] = (uint8_t)(value * 127.0f + 128.0f);
        break;
    case 16:
    {
        int16_t sample = (int16_t)(value < 0.0f ? value * 32768.0f : value * 32767.0f);
        memcpy(destination, &sample, sizeof(sample));
        break;
    }
    case 24:
    {
        int32_t sample = (int32_t)(value < 0.0f ? value * 8388608.0f : value * 8388607.0f);
        destination[0] = (uint8_t)sample;
        destination[1] = (uint8_t)(sample >> 8);
        destination[2] = (uint8_t)(sample >> 16);
        break;
    }
    case 32:
    {
        int32_t sample = (int32_t)(value < 0.0f
            ? (double)value * 2147483648.0
            : (double)value * 2147483647.0);
        memcpy(destination, &sample, sizeof(sample));
        break;
    }
    default:
        break;
    }
}

static HRESULT create_default_audio_client(EDataFlow flow, IMMDevice **device, IAudioClient **client)
{
    IMMDeviceEnumerator *enumerator = NULL;
    HRESULT result;

    result = CoCreateInstance(&LAN_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
        &LAN_IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(result))
    {
        return result;
    }

    result = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow, eConsole, device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(result))
    {
        return result;
    }

    result = IMMDevice_Activate(*device, &LAN_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)client);
    if (FAILED(result))
    {
        IMMDevice_Release(*device);
        *device = NULL;
    }
    return result;
}

AudioCapture *audio_capture_open(int loopback, char *error, unsigned int error_size)
{
    AudioCapture *capture = (AudioCapture *)calloc(1, sizeof(*capture));
    HRESULT result;

    if (capture == NULL)
    {
        if (error != NULL && error_size > 0)
        {
            (void)snprintf(error, error_size, "Not enough memory for audio capture");
        }
        return NULL;
    }

    result = create_default_audio_client(loopback ? eRender : eCapture, &capture->device, &capture->client);
    if (FAILED(result))
    {
        set_error(error, error_size, loopback
            ? "Opening the default playback endpoint (loopback needs a render device)"
            : "Opening the default microphone endpoint", result);
        audio_capture_close(capture);
        return NULL;
    }

    result = IAudioClient_GetMixFormat(capture->client, &capture->format);
    if (FAILED(result))
    {
        set_error(error, error_size, "Reading the capture endpoint format", result);
        audio_capture_close(capture);
        return NULL;
    }
    if (!describe_format(capture->format, &capture->sample_format, &capture->bytes_per_sample))
    {
        if (error != NULL && error_size > 0)
        {
            (void)snprintf(error, error_size, "Unsupported capture format: expected PCM or 32-bit float");
        }
        audio_capture_close(capture);
        return NULL;
    }

    capture->sample_rate = capture->format->nSamplesPerSec;
    capture->channels = capture->format->nChannels;
    capture->bits_per_sample = capture->format->wBitsPerSample;
    result = IAudioClient_Initialize(capture->client, AUDCLNT_SHAREMODE_SHARED,
        loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0, AUDIO_CLIENT_BUFFER_HNS, 0, capture->format, NULL);
    if (FAILED(result))
    {
        set_error(error, error_size, loopback
            ? "Starting system playback loopback capture"
            : "Starting microphone capture", result);
        audio_capture_close(capture);
        return NULL;
    }

    result = IAudioClient_GetService(capture->client, &LAN_IID_IAudioCaptureClient,
        (void **)&capture->capture_client);
    if (FAILED(result))
    {
        set_error(error, error_size, "Opening the WASAPI capture stream", result);
        audio_capture_close(capture);
        return NULL;
    }

    result = IAudioClient_Start(capture->client);
    if (FAILED(result))
    {
        set_error(error, error_size, "Starting the WASAPI capture stream", result);
        audio_capture_close(capture);
        return NULL;
    }

    return capture;
}

static float capture_channel(const uint8_t *data, uint32_t frame, uint16_t channel,
    const AudioCapture *capture, int silent)
{
    const uint8_t *sample;

    if (silent)
    {
        return 0.0f;
    }
    sample = data + (size_t)frame * capture->format->nBlockAlign
        + (size_t)channel * capture->bytes_per_sample;
    return decode_sample(sample, capture->sample_format, capture->bits_per_sample);
}

static int16_t float_to_pcm16(float value)
{
    if (!isfinite(value))
    {
        return 0;
    }
    if (value >= 1.0f)
    {
        return INT16_MAX;
    }
    if (value <= -1.0f)
    {
        return INT16_MIN;
    }
    return (int16_t)(value < 0.0f ? value * 32768.0f : value * 32767.0f);
}

int audio_capture_read(AudioCapture *capture, AudioFrameCallback callback, void *context,
    char *error, unsigned int error_size)
{
    UINT32 packet_frames = 0;
    HRESULT result;
    int16_t output[CAPTURE_OUTPUT_FRAMES * LAN_AUDIO_CHANNELS];
    uint32_t output_frames = 0;
    const double input_step = (double)capture->sample_rate / LAN_AUDIO_SAMPLE_RATE;

    result = IAudioCaptureClient_GetNextPacketSize(capture->capture_client, &packet_frames);
    if (FAILED(result))
    {
        set_error(error, error_size, "Checking for captured audio", result);
        return 0;
    }

    while (packet_frames != 0)
    {
        BYTE *data = NULL;
        UINT32 frames = 0;
        DWORD flags = 0;

        result = IAudioCaptureClient_GetBuffer(capture->capture_client, &data, &frames,
            &flags, NULL, NULL);
        if (FAILED(result))
        {
            set_error(error, error_size, "Reading captured audio", result);
            return 0;
        }

        while (capture->resample_position < (double)frames)
        {
            float output_left;
            float output_right;
            double fraction;
            uint32_t source_index;

            if (capture->resample_position < 0.0)
            {
                if (!capture->has_previous)
                {
                    capture->resample_position += input_step;
                    continue;
                }
                fraction = capture->resample_position + 1.0;
                output_left = capture->previous_left
                    + (capture_channel(data, 0, 0, capture, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                        - capture->previous_left) * (float)fraction;
                output_right = capture->previous_right
                    + (capture_channel(data, 0, capture->channels > 1 ? 1 : 0, capture,
                        (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) - capture->previous_right)
                        * (float)fraction;
            }
            else
            {
                source_index = (uint32_t)capture->resample_position;
                if (source_index + 1 >= frames)
                {
                    break;
                }
                fraction = capture->resample_position - (double)source_index;
                output_left = capture_channel(data, source_index, 0, capture,
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                output_left += (capture_channel(data, source_index + 1, 0, capture,
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) - output_left) * (float)fraction;
                output_right = capture_channel(data, source_index, capture->channels > 1 ? 1 : 0,
                    capture, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
                output_right += (capture_channel(data, source_index + 1,
                    capture->channels > 1 ? 1 : 0, capture,
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) - output_right) * (float)fraction;
            }

            output[output_frames * 2] = float_to_pcm16(output_left);
            output[output_frames * 2 + 1] = float_to_pcm16(output_right);
            ++output_frames;
            if (output_frames == CAPTURE_OUTPUT_FRAMES)
            {
                callback(output, output_frames, context);
                output_frames = 0;
            }
            capture->resample_position += input_step;
        }

        if (frames > 0)
        {
            capture->previous_left = capture_channel(data, frames - 1, 0, capture,
                (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            capture->previous_right = capture->channels > 1
                ? capture_channel(data, frames - 1, 1, capture, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
                : capture->previous_left;
            capture->has_previous = 1;
            capture->resample_position -= (double)frames;
        }

        result = IAudioCaptureClient_ReleaseBuffer(capture->capture_client, frames);
        if (FAILED(result))
        {
            set_error(error, error_size, "Releasing captured audio", result);
            return 0;
        }

        result = IAudioCaptureClient_GetNextPacketSize(capture->capture_client, &packet_frames);
        if (FAILED(result))
        {
            set_error(error, error_size, "Checking for captured audio", result);
            return 0;
        }
    }

    if (output_frames != 0)
    {
        callback(output, output_frames, context);
    }
    return 1;
}

void audio_capture_close(AudioCapture *capture)
{
    if (capture == NULL)
    {
        return;
    }
    if (capture->client != NULL)
    {
        (void)IAudioClient_Stop(capture->client);
    }
    if (capture->capture_client != NULL)
    {
        IAudioCaptureClient_Release(capture->capture_client);
    }
    if (capture->client != NULL)
    {
        IAudioClient_Release(capture->client);
    }
    if (capture->device != NULL)
    {
        IMMDevice_Release(capture->device);
    }
    if (capture->format != NULL)
    {
        CoTaskMemFree(capture->format);
    }
    free(capture);
}

AudioRender *audio_render_open(char *error, unsigned int error_size)
{
    AudioRender *render = (AudioRender *)calloc(1, sizeof(*render));
    HRESULT result;

    if (render == NULL)
    {
        if (error != NULL && error_size > 0)
        {
            (void)snprintf(error, error_size, "Not enough memory for audio playback");
        }
        return NULL;
    }

    result = create_default_audio_client(eRender, &render->device, &render->client);
    if (FAILED(result))
    {
        set_error(error, error_size, "Opening the default playback endpoint", result);
        audio_render_close(render);
        return NULL;
    }
    result = IAudioClient_GetMixFormat(render->client, &render->format);
    if (FAILED(result))
    {
        set_error(error, error_size, "Reading the playback endpoint format", result);
        audio_render_close(render);
        return NULL;
    }
    if (!describe_format(render->format, &render->sample_format, &render->bytes_per_sample))
    {
        if (error != NULL && error_size > 0)
        {
            (void)snprintf(error, error_size, "Unsupported playback format: expected PCM or 32-bit float");
        }
        audio_render_close(render);
        return NULL;
    }

    render->sample_rate = render->format->nSamplesPerSec;
    render->channels = render->format->nChannels;
    render->bits_per_sample = render->format->wBitsPerSample;
    result = IAudioClient_Initialize(render->client, AUDCLNT_SHAREMODE_SHARED, 0,
        AUDIO_CLIENT_BUFFER_HNS, 0, render->format, NULL);
    if (FAILED(result))
    {
        set_error(error, error_size, "Initializing WASAPI playback", result);
        audio_render_close(render);
        return NULL;
    }
    result = IAudioClient_GetService(render->client, &LAN_IID_IAudioRenderClient,
        (void **)&render->render_client);
    if (FAILED(result))
    {
        set_error(error, error_size, "Opening the WASAPI playback stream", result);
        audio_render_close(render);
        return NULL;
    }
    return render;
}

int audio_render_start(AudioRender *render, char *error, unsigned int error_size)
{
    HRESULT result = IAudioClient_Start(render->client);
    if (FAILED(result))
    {
        set_error(error, error_size, "Starting WASAPI playback", result);
        return 0;
    }
    render->started = 1;
    return 1;
}

static float resampled_render_value(AudioRender *render, int right, AudioSampleReader reader,
    void *context)
{
    int16_t *current = right ? &render->current_right : &render->current_left;
    int16_t *next = right ? &render->next_right : &render->next_left;
    float value;

    if (!render->have_current)
    {
        if (!reader(&render->current_left, &render->current_right, context))
        {
            render->current_left = 0;
            render->current_right = 0;
        }
        render->have_current = 1;
    }
    if (!render->have_next)
    {
        if (!reader(&render->next_left, &render->next_right, context))
        {
            render->next_left = 0;
            render->next_right = 0;
        }
        render->have_next = 1;
    }

    value = (float)*current + ((float)*next - (float)*current) * (float)render->resample_position;
    if (right == 1)
    {
        render->resample_position += (double)LAN_AUDIO_SAMPLE_RATE / render->sample_rate;
        while (render->resample_position >= 1.0)
        {
            render->current_left = render->next_left;
            render->current_right = render->next_right;
            if (!reader(&render->next_left, &render->next_right, context))
            {
                render->next_left = 0;
                render->next_right = 0;
            }
            render->resample_position -= 1.0;
        }
    }
    return value / 32768.0f;
}

int audio_render_write(AudioRender *render, AudioSampleReader reader, void *context,
    char *error, unsigned int error_size)
{
    UINT32 padding = 0;
    UINT32 buffer_frames = 0;
    UINT32 available;
    BYTE *data = NULL;
    UINT32 frame;
    HRESULT result;

    result = IAudioClient_GetCurrentPadding(render->client, &padding);
    if (FAILED(result))
    {
        set_error(error, error_size, "Checking playback buffer space", result);
        return 0;
    }
    result = IAudioClient_GetBufferSize(render->client, &buffer_frames);
    if (FAILED(result))
    {
        set_error(error, error_size, "Checking playback buffer size", result);
        return 0;
    }
    if (padding >= buffer_frames)
    {
        return 1;
    }
    available = buffer_frames - padding;
    result = IAudioRenderClient_GetBuffer(render->render_client, available, &data);
    if (FAILED(result))
    {
        set_error(error, error_size, "Requesting playback buffer", result);
        return 0;
    }

    memset(data, 0, (size_t)available * render->format->nBlockAlign);
    for (frame = 0; frame < available; ++frame)
    {
        float left = resampled_render_value(render, 0, reader, context);
        float right = resampled_render_value(render, 1, reader, context);
        uint16_t channel;

        if (render->channels == 1)
        {
            encode_sample(data + (size_t)frame * render->format->nBlockAlign,
                render->sample_format, render->bits_per_sample, (left + right) * 0.5f);
        }
        else
        {
            for (channel = 0; channel < render->channels; ++channel)
            {
                float value = channel == 0 ? left : (channel == 1 ? right : 0.0f);
                encode_sample(data + (size_t)frame * render->format->nBlockAlign
                    + (size_t)channel * render->bytes_per_sample,
                    render->sample_format, render->bits_per_sample, value);
            }
        }
    }

    result = IAudioRenderClient_ReleaseBuffer(render->render_client, available, 0);
    if (FAILED(result))
    {
        set_error(error, error_size, "Submitting playback audio", result);
        return 0;
    }
    return 1;
}

void audio_render_close(AudioRender *render)
{
    if (render == NULL)
    {
        return;
    }
    if (render->client != NULL && render->started)
    {
        (void)IAudioClient_Stop(render->client);
    }
    if (render->render_client != NULL)
    {
        IAudioRenderClient_Release(render->render_client);
    }
    if (render->client != NULL)
    {
        IAudioClient_Release(render->client);
    }
    if (render->device != NULL)
    {
        IMMDevice_Release(render->device);
    }
    if (render->format != NULL)
    {
        CoTaskMemFree(render->format);
    }
    free(render);
}
