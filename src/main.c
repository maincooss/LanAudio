#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mmsystem.h>
#include <objbase.h>

#include "audio_win.h"
#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAN_AUDIO_DEFAULT_PORT 49321u
#define JITTER_CAPACITY 64u
#define JITTER_START_PACKETS 4u
#define RECEIVE_BUFFER_SIZE 2048

typedef struct JitterSlot
{
    uint32_t sequence;
    int valid;
    int16_t samples[LAN_AUDIO_SAMPLE_COUNT];
} JitterSlot;

typedef struct JitterBuffer
{
    JitterSlot slots[JITTER_CAPACITY];
    struct sockaddr_in peer;
    uint32_t expected_sequence;
    uint32_t sample_offset;
    unsigned int packet_count;
    int initialized;
    int warned_other_sender;
} JitterBuffer;

typedef struct SenderContext
{
    SOCKET socket_handle;
    struct sockaddr_in destination;
    int16_t pending_samples[LAN_AUDIO_SAMPLE_COUNT];
    uint32_t pending_frames;
    uint32_t sequence;
    int failed;
} SenderContext;

static volatile LONG g_running = 1;

static BOOL WINAPI console_handler(DWORD signal)
{
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT
        || signal == CTRL_CLOSE_EVENT || signal == CTRL_LOGOFF_EVENT
        || signal == CTRL_SHUTDOWN_EVENT)
    {
        InterlockedExchange(&g_running, 0);
        return TRUE;
    }
    return FALSE;
}

static void print_usage(void)
{
    puts("LAN Audio - Windows 局域网实时音频");
    puts("");
    puts("用法:");
    puts("  LanAudio.exe");
    puts("  LanAudio.exe interactive");
    puts("  LanAudio.exe send --source loopback|mic --target IPv4 [--port 49321]");
    puts("  LanAudio.exe receive [--bind IPv4] [--port 49321]");
    puts("  LanAudio.exe --self-test");
    puts("");
    puts("示例:");
    puts("  LanAudio.exe receive --port 49321");
    puts("  LanAudio.exe send --source loopback --target 192.168.1.42");
    puts("  LanAudio.exe send --source mic --target 192.168.1.255");
    puts("");
    puts("接收端默认监听所有 IPv4 网卡并播放到 Windows 默认播放设备。");
    puts("loopback 捕获系统播放声音；mic 捕获默认麦克风。");
}

static int read_console_line(const char *prompt, char *buffer, size_t buffer_size)
{
    size_t length;
    int character;

    printf("%s", prompt);
    (void)fflush(stdout);
    if (fgets(buffer, (int)buffer_size, stdin) == NULL)
    {
        return 0;
    }

    length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == '\n')
    {
        buffer[--length] = '\0';
    }
    else if (!feof(stdin))
    {
        while ((character = getchar()) != '\n' && character != EOF)
        {
        }
        fputs("输入过长，请重试。\n", stderr);
        return -1;
    }
    if (length > 0 && buffer[length - 1] == '\r')
    {
        buffer[--length] = '\0';
    }
    return 1;
}

static int parse_port(const char *text, unsigned short *port)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || text[0] == '\0')
    {
        return 0;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > 65535)
    {
        return 0;
    }
    *port = (unsigned short)value;
    return 1;
}

static int parse_ipv4(const char *text, struct in_addr *address)
{
    return text != NULL && InetPtonA(AF_INET, text, address) == 1;
}

static int configure_interactive(const char **role, const char **target, const char **source,
    const char **bind_text, unsigned short *port)
{
    static char target_storage[INET_ADDRSTRLEN];
    char input[128];
    struct in_addr address;
    int read_result;

    puts("LAN Audio - 交互模式");
    puts("1. 接收端（直接监听默认端口 49321）");
    puts("2. 发送端");
    puts("0. 退出");
    for (;;)
    {
        read_result = read_console_line("请选择 [1/2/0]: ", input, sizeof(input));
        if (read_result == 0)
        {
            return 0;
        }
        if (read_result < 0)
        {
            continue;
        }
        if (strcmp(input, "0") == 0)
        {
            return 0;
        }
        if (strcmp(input, "1") == 0)
        {
            *role = "receive";
            *target = NULL;
            *source = NULL;
            *bind_text = "0.0.0.0";
            *port = LAN_AUDIO_DEFAULT_PORT;
            return 1;
        }
        if (strcmp(input, "2") == 0)
        {
            *role = "send";
            *bind_text = "0.0.0.0";
            *port = LAN_AUDIO_DEFAULT_PORT;
            break;
        }
        puts("请输入 1、2 或 0。");
    }

    puts("选择音频来源:");
    puts("1. 系统播放声音（loopback）");
    puts("2. 麦克风（mic）");
    for (;;)
    {
        read_result = read_console_line("请选择 [1/2]: ", input, sizeof(input));
        if (read_result == 0)
        {
            return 0;
        }
        if (read_result < 0)
        {
            continue;
        }
        if (strcmp(input, "1") == 0)
        {
            *source = "loopback";
            break;
        }
        if (strcmp(input, "2") == 0)
        {
            *source = "mic";
            break;
        }
        puts("请输入 1 或 2。");
    }

    for (;;)
    {
        read_result = read_console_line("请输入接收端 IPv4 地址（单播或广播）: ", input, sizeof(input));
        if (read_result == 0)
        {
            return 0;
        }
        if (read_result < 0)
        {
            continue;
        }
        if (parse_ipv4(input, &address))
        {
            (void)snprintf(target_storage, sizeof(target_storage), "%s", input);
            *target = target_storage;
            break;
        }
        puts("IPv4 地址无效，请重新输入（例如 192.168.1.42）。");
    }

    for (;;)
    {
        printf("请输入 UDP 端口 [默认 %u]: ", LAN_AUDIO_DEFAULT_PORT);
        read_result = read_console_line("", input, sizeof(input));
        if (read_result == 0)
        {
            *target = NULL;
            return 0;
        }
        if (read_result < 0)
        {
            continue;
        }
        if (input[0] == '\0')
        {
            *port = LAN_AUDIO_DEFAULT_PORT;
            break;
        }
        if (parse_port(input, port))
        {
            break;
        }
        puts("端口无效，请输入 1 到 65535 之间的数字，或直接回车使用默认端口。");
    }

    printf("即将发送：来源=%s，目标=%s:%u\n", *source, *target, *port);
    return 1;
}

static void print_local_ipv4_addresses(void)
{
    ULONG buffer_size = 15000;
    ULONG result;
    PIP_ADAPTER_ADDRESSES adapters = (PIP_ADAPTER_ADDRESSES)malloc(buffer_size);
    PIP_ADAPTER_ADDRESSES adapter;
    unsigned int printed = 0;

    if (adapters == NULL)
    {
        fputs("无法分配内存以枚举本机网络地址。\n", stderr);
        return;
    }
    result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        NULL, adapters, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW)
    {
        PIP_ADAPTER_ADDRESSES larger = (PIP_ADAPTER_ADDRESSES)realloc(adapters, buffer_size);
        if (larger == NULL)
        {
            free(adapters);
            fputs("无法分配内存以枚举本机网络地址。\n", stderr);
            return;
        }
        adapters = larger;
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            NULL, adapters, &buffer_size);
    }
    if (result != NO_ERROR)
    {
        fprintf(stderr, "枚举本机 IPv4 地址失败: %lu\n", (unsigned long)result);
        free(adapters);
        return;
    }

    printf("本机 IPv4 地址（选择与发送端同一局域网的地址）:");
    for (adapter = adapters; adapter != NULL; adapter = adapter->Next)
    {
        PIP_ADAPTER_UNICAST_ADDRESS unicast;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->OperStatus != IfOperStatusUp)
        {
            continue;
        }
        for (unicast = adapter->FirstUnicastAddress; unicast != NULL; unicast = unicast->Next)
        {
            char text[INET_ADDRSTRLEN];
            const struct sockaddr_in *address;
            if (unicast->Address.lpSockaddr == NULL || unicast->Address.lpSockaddr->sa_family != AF_INET)
            {
                continue;
            }
            address = (const struct sockaddr_in *)unicast->Address.lpSockaddr;
            if (InetNtopA(AF_INET, (void *)&address->sin_addr, text, sizeof(text)) != NULL)
            {
                printf(" %s", text);
                ++printed;
            }
        }
    }
    putchar('\n');
    if (printed == 0)
    {
        puts("未发现已连接的 IPv4 网络接口；可在接收端运行 ipconfig 查询地址。");
    }
    free(adapters);
}

static int socket_startup(void)
{
    WSADATA data;
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
    {
        fprintf(stderr, "Winsock 初始化失败: %d\n", result);
        return 0;
    }
    return 1;
}

static void sender_send_frames(const int16_t *samples, uint32_t frame_count, void *context)
{
    SenderContext *sender = (SenderContext *)context;
    uint32_t offset = 0;

    while (offset < frame_count && InterlockedCompareExchange(&g_running, 1, 1) != 0)
    {
        uint32_t available = LAN_AUDIO_FRAMES_PER_PACKET - sender->pending_frames;
        uint32_t copy_frames = frame_count - offset;
        int sent;

        if (copy_frames > available)
        {
            copy_frames = available;
        }
        memcpy(sender->pending_samples + sender->pending_frames * LAN_AUDIO_CHANNELS,
            samples + offset * LAN_AUDIO_CHANNELS,
            (size_t)copy_frames * LAN_AUDIO_CHANNELS * sizeof(int16_t));
        sender->pending_frames += copy_frames;
        offset += copy_frames;

        if (sender->pending_frames != LAN_AUDIO_FRAMES_PER_PACKET)
        {
            continue;
        }

        {
            uint8_t packet[LAN_AUDIO_MAX_PACKET_SIZE];
            int packet_size = protocol_encode(packet, sizeof(packet), sender->sequence++,
                sender->pending_samples);
            if (packet_size == 0)
            {
                fputs("内部错误：音频包编码失败\n", stderr);
                sender->failed = 1;
                InterlockedExchange(&g_running, 0);
                return;
            }
            sent = sendto(sender->socket_handle, (const char *)packet, packet_size, 0,
                (const struct sockaddr *)&sender->destination, (int)sizeof(sender->destination));
        }
        if (sent == SOCKET_ERROR)
        {
            fprintf(stderr, "UDP 发送失败: %d\n", WSAGetLastError());
            sender->failed = 1;
            InterlockedExchange(&g_running, 0);
            return;
        }
        sender->pending_frames = 0;
    }
}

static int run_sender(const char *target_text, const char *source, unsigned short port)
{
    struct sockaddr_in destination;
    SOCKET socket_handle = INVALID_SOCKET;
    int broadcast_enabled = 1;
    AudioCapture *capture = NULL;
    SenderContext sender;
    char error[256];
    int exit_code = 1;
    HRESULT com_result;

    memset(&destination, 0, sizeof(destination));
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    if (!parse_ipv4(target_text, &destination.sin_addr))
    {
        fprintf(stderr, "目标地址不是有效的 IPv4 地址: %s\n", target_text);
        return 2;
    }

    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET)
    {
        fprintf(stderr, "创建 UDP socket 失败: %d\n", WSAGetLastError());
        return 1;
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
        (const char *)&broadcast_enabled, (int)sizeof(broadcast_enabled)) == SOCKET_ERROR)
    {
        fprintf(stderr, "启用 UDP 广播失败: %d\n", WSAGetLastError());
        closesocket(socket_handle);
        return 1;
    }

    com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com_result))
    {
        fprintf(stderr, "COM 初始化失败 (HRESULT 0x%08lX)\n", (unsigned long)com_result);
        closesocket(socket_handle);
        return 1;
    }

    memset(error, 0, sizeof(error));
    capture = audio_capture_open(strcmp(source, "loopback") == 0, error, sizeof(error));
    if (capture == NULL)
    {
        fprintf(stderr, "音频采集初始化失败: %s\n", error);
        CoUninitialize();
        closesocket(socket_handle);
        return 1;
    }

    memset(&sender, 0, sizeof(sender));
    sender.socket_handle = socket_handle;
    sender.destination = destination;
    printf("发送端已启动：来源=%s，目标=%s:%u，48 kHz / 16-bit / stereo\n",
        source, target_text, port);
    puts("按 Ctrl+C 停止。");
    exit_code = 0;
    while (InterlockedCompareExchange(&g_running, 1, 1) != 0)
    {
        if (!audio_capture_read(capture, sender_send_frames, &sender, error, sizeof(error)))
        {
            fprintf(stderr, "音频采集失败: %s\n", error);
            exit_code = 1;
            break;
        }
        Sleep(1);
    }
    if (sender.failed)
    {
        exit_code = 1;
    }

    audio_capture_close(capture);
    CoUninitialize();
    closesocket(socket_handle);
    return exit_code;
}

static int jitter_insert(JitterBuffer *jitter, const struct sockaddr_in *sender,
    uint32_t sequence, const int16_t *samples)
{
    uint32_t distance;
    JitterSlot *slot;
    unsigned int index;

    if (!jitter->initialized)
    {
        jitter->peer = *sender;
        jitter->expected_sequence = sequence;
        jitter->initialized = 1;
    }
    else if (jitter->peer.sin_addr.s_addr != sender->sin_addr.s_addr
        || jitter->peer.sin_port != sender->sin_port)
    {
        if (!jitter->warned_other_sender)
        {
            puts("忽略来自其他发送端的音频包；每个接收进程仅播放一个发送端。");
            jitter->warned_other_sender = 1;
        }
        return 0;
    }

    distance = sequence - jitter->expected_sequence;
    if (distance >= 0x80000000u || distance >= JITTER_CAPACITY)
    {
        return 0;
    }

    index = sequence % JITTER_CAPACITY;
    slot = &jitter->slots[index];
    if (slot->valid && slot->sequence == sequence)
    {
        return 0;
    }
    if (slot->valid)
    {
        --jitter->packet_count;
    }
    slot->sequence = sequence;
    slot->valid = 1;
    memcpy(slot->samples, samples, sizeof(slot->samples));
    ++jitter->packet_count;
    return 1;
}

static unsigned int jitter_contiguous_packets(const JitterBuffer *jitter)
{
    unsigned int count = 0;
    while (count < JITTER_CAPACITY)
    {
        uint32_t sequence = jitter->expected_sequence + count;
        const JitterSlot *slot = &jitter->slots[sequence % JITTER_CAPACITY];
        if (!slot->valid || slot->sequence != sequence)
        {
            break;
        }
        ++count;
    }
    return count;
}

static int jitter_read_sample(int16_t *left, int16_t *right, void *context)
{
    JitterBuffer *jitter = (JitterBuffer *)context;
    JitterSlot *slot = &jitter->slots[jitter->expected_sequence % JITTER_CAPACITY];

    if (slot->valid && slot->sequence == jitter->expected_sequence)
    {
        *left = slot->samples[jitter->sample_offset * 2];
        *right = slot->samples[jitter->sample_offset * 2 + 1];
    }
    else
    {
        *left = 0;
        *right = 0;
    }

    ++jitter->sample_offset;
    if (jitter->sample_offset == LAN_AUDIO_FRAMES_PER_PACKET)
    {
        if (slot->valid && slot->sequence == jitter->expected_sequence)
        {
            slot->valid = 0;
            --jitter->packet_count;
        }
        ++jitter->expected_sequence;
        jitter->sample_offset = 0;
    }
    return 1;
}

static int run_receiver(const char *bind_text, unsigned short port)
{
    SOCKET socket_handle = INVALID_SOCKET;
    struct sockaddr_in local_address;
    AudioRender *render = NULL;
    JitterBuffer *jitter = NULL;
    u_long nonblocking = 1;
    char error[256];
    int exit_code = 1;
    int render_started = 0;
    int warned_invalid_packet = 0;
    HRESULT com_result;

    memset(&local_address, 0, sizeof(local_address));
    local_address.sin_family = AF_INET;
    local_address.sin_port = htons(port);
    if (!parse_ipv4(bind_text, &local_address.sin_addr))
    {
        fprintf(stderr, "监听地址不是有效的 IPv4 地址: %s\n", bind_text);
        return 2;
    }

    socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET)
    {
        fprintf(stderr, "创建 UDP socket 失败: %d\n", WSAGetLastError());
        return 1;
    }
    if (bind(socket_handle, (const struct sockaddr *)&local_address,
        (int)sizeof(local_address)) == SOCKET_ERROR)
    {
        fprintf(stderr, "绑定 UDP %s:%u 失败: %d\n", bind_text, port, WSAGetLastError());
        closesocket(socket_handle);
        return 1;
    }
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) == SOCKET_ERROR)
    {
        fprintf(stderr, "设置 UDP socket 非阻塞失败: %d\n", WSAGetLastError());
        closesocket(socket_handle);
        return 1;
    }

    com_result = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(com_result))
    {
        fprintf(stderr, "COM 初始化失败 (HRESULT 0x%08lX)\n", (unsigned long)com_result);
        closesocket(socket_handle);
        return 1;
    }

    memset(error, 0, sizeof(error));
    render = audio_render_open(error, sizeof(error));
    if (render == NULL)
    {
        fprintf(stderr, "音频播放初始化失败: %s\n", error);
        CoUninitialize();
        closesocket(socket_handle);
        return 1;
    }
    jitter = (JitterBuffer *)calloc(1, sizeof(*jitter));
    if (jitter == NULL)
    {
        fputs("内存不足，无法创建接收缓冲区\n", stderr);
        audio_render_close(render);
        CoUninitialize();
        closesocket(socket_handle);
        return 1;
    }

    print_local_ipv4_addresses();
    printf("接收端已启动：监听 %s:%u，播放到 Windows 默认播放设备。\n", bind_text, port);
    puts("等待音频发送端；按 Ctrl+C 停止。");
    exit_code = 0;
    while (InterlockedCompareExchange(&g_running, 1, 1) != 0)
    {
        for (;;)
        {
            uint8_t packet[RECEIVE_BUFFER_SIZE];
            int16_t samples[LAN_AUDIO_SAMPLE_COUNT];
            struct sockaddr_in sender;
            int sender_size = (int)sizeof(sender);
            int received = recvfrom(socket_handle, (char *)packet, sizeof(packet), 0,
                (struct sockaddr *)&sender, &sender_size);

            if (received == SOCKET_ERROR)
            {
                int socket_error = WSAGetLastError();
                if (socket_error != WSAEWOULDBLOCK)
                {
                    fprintf(stderr, "UDP 接收失败: %d\n", socket_error);
                    InterlockedExchange(&g_running, 0);
                    exit_code = 1;
                }
                break;
            }

            {
                uint32_t sequence;
                if (!protocol_decode(packet, (size_t)received, &sequence, samples))
                {
                    if (!warned_invalid_packet)
                    {
                        puts("忽略无效或格式不兼容的 UDP 音频包。");
                        warned_invalid_packet = 1;
                    }
                    continue;
                }
                (void)jitter_insert(jitter, &sender, sequence, samples);
            }
        }

        if (!render_started && jitter_contiguous_packets(jitter) >= JITTER_START_PACKETS)
        {
            char peer_address[INET_ADDRSTRLEN];
            if (!audio_render_start(render, error, sizeof(error)))
            {
                fprintf(stderr, "音频播放失败: %s\n", error);
                exit_code = 1;
                break;
            }
            if (InetNtopA(AF_INET, &jitter->peer.sin_addr, peer_address, sizeof(peer_address)) == NULL)
            {
                (void)snprintf(peer_address, sizeof(peer_address), "未知");
            }
            printf("已连接发送端 %s；抖动缓冲约 %u ms。\n",
                peer_address, JITTER_START_PACKETS * 5);
            render_started = 1;
        }
        if (render_started && !audio_render_write(render, jitter_read_sample, jitter,
            error, sizeof(error)))
        {
            fprintf(stderr, "音频播放失败: %s\n", error);
            exit_code = 1;
            break;
        }
        Sleep(1);
    }
    free(jitter);
    audio_render_close(render);
    CoUninitialize();
    closesocket(socket_handle);
    return exit_code;
}

static int parse_options(int argc, char **argv, const char **role, const char **target,
    const char **source, const char **bind_text, unsigned short *port)
{
    int index;

    if (argc < 2)
    {
        print_usage();
        return 0;
    }
    *role = argv[1];
    *target = NULL;
    *source = NULL;
    *bind_text = "0.0.0.0";
    *port = LAN_AUDIO_DEFAULT_PORT;

    if (strcmp(*role, "--help") == 0 || strcmp(*role, "-h") == 0)
    {
        print_usage();
        return 0;
    }
    if (strcmp(*role, "--version") == 0)
    {
        puts("LAN Audio 1.0.0 (C17, WASAPI, UDP)");
        return 0;
    }
    if (strcmp(*role, "--self-test") == 0)
    {
        if (argc != 2)
        {
            fputs("--self-test 不接受其他参数\n", stderr);
            return -1;
        }
        if (!protocol_self_test())
        {
            fputs("协议自检失败\n", stderr);
            return -1;
        }
        puts("协议自检通过");
        return 0;
    }
    if (strcmp(*role, "send") != 0 && strcmp(*role, "receive") != 0)
    {
        fprintf(stderr, "未知模式: %s\n", *role);
        print_usage();
        return -1;
    }

    for (index = 2; index < argc; ++index)
    {
        const char *option = argv[index];
        const char *value;
        if (index + 1 >= argc)
        {
            fprintf(stderr, "选项缺少值: %s\n", option);
            return -1;
        }
        value = argv[++index];
        if (strcmp(option, "--target") == 0 && strcmp(*role, "send") == 0)
        {
            *target = value;
        }
        else if (strcmp(option, "--source") == 0 && strcmp(*role, "send") == 0)
        {
            *source = value;
        }
        else if (strcmp(option, "--bind") == 0 && strcmp(*role, "receive") == 0)
        {
            *bind_text = value;
        }
        else if (strcmp(option, "--port") == 0)
        {
            if (!parse_port(value, port))
            {
                fprintf(stderr, "无效端口号: %s\n", value);
                return -1;
            }
        }
        else
        {
            fprintf(stderr, "未知或不适用于此模式的选项: %s\n", option);
            return -1;
        }
    }

    if (strcmp(*role, "send") == 0)
    {
        if (*target == NULL || *source == NULL)
        {
            fputs("发送模式必须指定 --source 和 --target\n", stderr);
            return -1;
        }
        if (strcmp(*source, "loopback") != 0 && strcmp(*source, "mic") != 0)
        {
            fputs("--source 只能是 loopback 或 mic\n", stderr);
            return -1;
        }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *role;
    const char *target;
    const char *source;
    const char *bind_text;
    unsigned short port;
    int options_result;
    int result;
    int timer_resolution_enabled = 0;

    (void)SetConsoleOutputCP(CP_UTF8);
    (void)SetConsoleCP(CP_UTF8);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "interactive") == 0))
    {
        options_result = configure_interactive(&role, &target, &source, &bind_text, &port);
        if (options_result <= 0)
        {
            return options_result == 0 ? 0 : 2;
        }
    }
    else
    {
        if (argc > 1 && strcmp(argv[1], "interactive") == 0)
        {
            fputs("交互模式不接受其他命令行参数。\n", stderr);
            return 2;
        }
        options_result = parse_options(argc, argv, &role, &target, &source, &bind_text, &port);
        if (options_result <= 0)
        {
            return options_result == 0 ? 0 : 2;
        }
    }
    if (!socket_startup())
    {
        return 1;
    }
    if (!SetConsoleCtrlHandler(console_handler, TRUE))
    {
        fputs("无法注册 Ctrl+C 处理程序\n", stderr);
        WSACleanup();
        return 1;
    }

    if (timeBeginPeriod(1) == TIMERR_NOERROR)
    {
        timer_resolution_enabled = 1;
    }
    else
    {
        fputs("警告：无法启用 1 ms 定时器精度，实时音频延迟可能增加。\n", stderr);
    }
    result = strcmp(role, "send") == 0
        ? run_sender(target, source, port)
        : run_receiver(bind_text, port);

    if (timer_resolution_enabled)
    {
        (void)timeEndPeriod(1);
    }
    (void)SetConsoleCtrlHandler(console_handler, FALSE);
    WSACleanup();
    return result;
}
