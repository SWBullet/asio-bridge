# Bridge — Windows 进程回环音频桥

把任意目标应用的渲染音频，经 **Windows 11 进程回环（Process Loopback）** 旁路采集，
通过 **RME MADIface ASIO** 直送 ADI-2 Pro DAC。免虚拟声卡、免第三方驱动、不占麦克风。

```
目标应用（播放器 / 浏览器 / 游戏）
   │  WASAPI 共享渲染（float32）
   ▼
Windows 音频引擎（audiodg）
   │  VAD\Process_Loopback 按 PID 旁路取流
   │  取点位置：会话处理之后、终点主音量之前（数值实验实测）
   ▼
Bridge（本程序）
   │  MTA 采集线程 → 无锁环形缓冲 → 速率锁 + 分数重采样 → ASIO 回调
   ▼
RME MADIface ASIO（ASIOSTFloat32LSB）→ ADI-2 Pro DAC → 耳机/音箱
```

## 特性

- **进程回环采集**：按 PID 锁定「当前最响的渲染进程」，目标切换/暂停/恢复自动跟随；
  发现逻辑自动排除桥自身的引擎回声会话。
- **自动端点静音（tap→redirect）**：自动静音目标进程所在渲染端点的终点主音量，
  消除「原声 + 桥副本」双重声（ASIO 不走 WDM 终点音量，捕获点在终点音量之前，
  两者均不受影响）；重建/退出时自动恢复。
- **双环速率锁**：QPC 时间戳对 + 最小二乘测两时钟真实速率 → ratio 基值；
  水位闭环微调；暂停/恢复断档自动截断回归窗口（60 秒短窗，500ms 断档检测）。
- **水位快排**：水位超 2.5× 目标时直接丢弃陈旧预滚帧，秒级回落。
- **内嵌 Web 控制台**（http://127.0.0.1:3999）：复古金属仪表风（拉丝金属铭牌、
  12AX7 电子管指示灯、机械按键/拨杆），实时水位示波、速率锁读数、链路 LED 状态。
- **TPDF 抖动**（默认开）、直通模式（ratio 恒 1.0，逐位直通）。
- **数值自检**（先测后上线）：`--resampler-test`、`--pll-test`、
  `--capture-test`（渲染 997Hz 正弦 → 按 PID 回环自采 → RMS/峰值/频率比对 +
  静音语义验证）。

## 构建

需要 VS2022（MSVC）+ CMake ≥3.20。ASIO SDK 2.3 已随仓库提供（见
`third_party/ASIOSDK2.3`，其许可为 Steinberg ASIO Licensing Agreement）。

```powershell
cmake -B build -S .
cmake --build build --config Release
```

## 使用

```powershell
# 列出 ASIO 驱动
.\build\Release\asio_bridge.exe --list

# 测试 ASIO 链路（1kHz 正弦直出）
.\build\Release\asio_bridge.exe --tone --rate 44100

# 数值自检（渲染→回环自采比对，含静音语义）
.\build\Release\asio_bridge.exe --capture-test

# 正式桥接：自动发现并锁定最响的渲染进程
.\build\Release\asio_bridge.exe
```

1. 启动 Bridge 后播放任意应用音频（首次运行请确认该应用有输出设备可用）。
2. Bridge 10~20 秒内自动锁定该进程（控制台「目标进程」可见 PID 与活跃态）。
3. 控制台 `http://127.0.0.1:3999` 提供水位下限、TPDF 抖动、直通模式、
   重建链路、重置统计等控制。
4. 后台自启：`tools\install_autostart.ps1`（计划任务 + 隐藏启动，
   示例参数见脚本）。

## 关键机制

- **取点位置**（`--capture-test` 阶段 4 实测）：回环取点在「会话静音之后、
  终点主音量之前」——因此用终点主音量静音做 redirect（会话静音会哑掉捕获）。
- **采集格式**：优先用目标端点真实引擎混音格式（float32 全链路位透明，
  无 AUTOCONVERTPCM）；不可用时回退 16bit/44.1k + 引擎转换。
- **速率锁窗口**：60 秒窗口 + 500ms 断档截断 + 15 秒清洁段要求，
  切歌后读数约 15 秒恢复（断档期间保持上一好值）。

## 已知限制

- 需要 Windows 11 22H2+（进程回环 API）。
- 端点静音作用域为整端点：该端点上其他 WDM 声音（系统提示音等）一并静音。
- 目标应用暂停时回环无数据包，短暂欠载由自适应水位吸收。

## 许可

本项目代码以 MIT 许可发布（见 `LICENSE`）。
`third_party/ASIOSDK2.3` 为 Steinberg ASIO SDK，按其随附许可协议分发。
