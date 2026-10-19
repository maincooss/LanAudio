# LAN Audio

LAN Audio 是一个 Windows 控制台程序，可将一台电脑上的系统播放声音或麦克风输入，通过局域网 UDP 实时传到一台或多台电脑播放。发送端和接收端使用同一个程序，既可通过交互菜单操作，也可使用 `send` / `receive` 命令行参数。

## 构建

- 使用 Visual Studio 2019 16.8 或更高版本，并安装“使用 C++ 的桌面开发”和 Windows 10/11 SDK。
- 打开 `LanAudio.sln`，选择 `x64` 和 `Release`，执行“生成解决方案”。
- 或在 Visual Studio Developer 命令提示符运行：

```powershell
msbuild LanAudio.sln /m /p:Configuration=Release /p:Platform=x64
```

生成文件位于 `x64\Release\LanAudio.exe`。项目代码是纯 C，使用 Windows SDK 自带的 WASAPI、COM、Winsock、IP Helper 和多媒体定时器 API；不依赖第三方库或驱动。项目设置为 C17。

本仓库构建环境中的 MSVC 为 19.51.36260。实际探测确认 `/std:c17` 可编译；`/std:c23` 和 `/std:c2x` 被该编译器当作未知选项忽略，因此项目使用此工具链能可靠构建的最新模式 C17，而不启用无法验证的 C23 特性。

可运行协议自检（无需音频设备）：

```powershell
.\x64\Release\LanAudio.exe --self-test
```

## 使用

### 交互模式

直接运行程序（不带参数）即可进入菜单；也可以显式运行 `LanAudio.exe interactive`。

```powershell
.\LanAudio.exe
```

选择“接收端”后，程序立即监听默认 UDP 端口 `49321` 并播放收到的声音。选择“发送端”后，按提示选择系统播放声音或麦克风，输入接收端 IPv4 地址，再输入端口；端口直接回车即可使用默认值。广播时，目标地址输入发送端所在网段的广播地址，例如 `192.168.1.255`。

先在接收端运行程序。它会列出本机 IPv4 地址；将与发送端处于同一局域网的地址输入发送端提示。

### 接收端

```powershell
.\LanAudio.exe receive
```

默认监听 `0.0.0.0:49321`，即所有 IPv4 网卡上的 UDP 49321 端口，并播放到 Windows 默认播放设备。也可指定监听地址和端口：

```powershell
.\LanAudio.exe receive --bind 192.168.1.42 --port 49321
```

### 发送系统播放声音

```powershell
.\LanAudio.exe send --source loopback --target 192.168.1.42
```

WASAPI loopback 捕获默认播放端点的系统混音，不需要虚拟声卡。只要 Windows 中存在可用的播放渲染端点，即使电脑没有接扬声器，通常也可以捕获路由到该端点的声音。

**如果发送端完全没有可用的播放端点，loopback 就无法启动。** 程序会报告端点/采集初始化错误，不会伪装成已开始传输。此时可在 Windows 启用一个真实音频端点，或安装/配置虚拟音频端点、哑音频设备；虚拟声卡不是本软件的依赖或附带组件。

### 发送麦克风声音

```powershell
.\LanAudio.exe send --source mic --target 192.168.1.42
```

`mic` 使用 Windows 默认录音端点。此模式不需要播放设备或虚拟声卡。若采集失败，请检查是否已连接/启用麦克风、Windows 麦克风隐私权限以及默认录音设备。

### 一对多广播

将目标设置为发送端所在子网的广播地址，例如常见 `/24` 子网的 `.255` 地址：

```powershell
.\LanAudio.exe send --source loopback --target 192.168.1.255
```

同一广播域中所有正在运行接收端的电脑都可播放。不同子网掩码的广播地址不一定以 `.255` 结尾；路由器、无线客户端隔离、VLAN 或防火墙也可能禁止广播。每个接收进程固定播放首先连入的一个发送端；关闭并重启接收端即可切换发送端。

## 参数

```text
LanAudio.exe
LanAudio.exe interactive
LanAudio.exe send --source loopback|mic --target IPv4 [--port 49321]
LanAudio.exe receive [--bind IPv4] [--port 49321]
LanAudio.exe --help
LanAudio.exe --version
LanAudio.exe --self-test
```

端口默认是 UDP `49321`；两端必须一致。Windows 防火墙首次询问时，允许程序在可信的“专用网络”上通信，或为所用端口添加入站 UDP 规则。按 `Ctrl+C` 退出。

## 音频和网络

- 捕获端点采用 WASAPI 共享模式和 Windows 默认端点混音格式；程序转换为 48 kHz、立体声、16-bit PCM。
- 音频按 5 ms 分帧，每个 UDP 数据报约 980 字节；接收端使用最多 64 帧的有界抖动缓冲，约 20 ms 连续数据后开始播放。程序请求 1 ms Windows 定时器精度以改善轮询延迟；实际端到端延迟还受 Windows 音频引擎、设备缓冲和网络影响。
- 未压缩音频约占 1.54 Mbit/s（不含网络协议开销）。网络拥塞或丢包时会出现短暂静音；UDP 不重传，以避免重传增加延迟。
- 传输为明文且不做身份认证，只建议在可信局域网使用。接收端只接受首个发送端的数据，避免把多个发送源混音。

命令行帮助：

```powershell
.\LanAudio.exe --help
```
