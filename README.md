# ASIO Bridge — Windows 进程回环音频桥

把任意目标应用的渲染音频，经 **Windows 11 进程回环（Process Loopback）** 旁路采集，
通过 **ASIO**（或 WASAPI 独占回退）直送 DAC。免虚拟声卡、免第三方驱动、不占麦克风。

```
目标应用（播放器 / 浏览器 / 游戏）
   │  WASAPI 共享渲染（float32）
   ▼
Windows 音频引擎（audiodg）
   │  VAD\Process_Loopback 按 PID 旁路取流
   │  取点位置：会话处理之后、终点主音量之前（数值实验实测）
   ▼
Bridge（本程序）
   │  MTA 采集线程 → 无锁环形缓冲 → 速率锁 + 分数重采样
   ▼
DSP 链（可选逐级开关）：
   │  300B 电子管染色 → 厚度与宽度（谐波预延时 + 高低切）→ 火箭推进器（湿增益）
   ▼
输出后端（自动选择）
   ├─ RME MADIface ASIO（ASIOSTFloat32LSB）→ ADI-2 Pro DAC
   └─ 其他设备 → WASAPI 独占渲染（事件驱动线程 + 候选格式探测）
```

---

## 功能总览

### 1. 进程回环采集

- 按 **PID 锁定「当前最响的渲染进程」**，目标切换 / 暂停 / 恢复自动跟随；
- 发现逻辑自动排除桥自身的引擎回声会话；
- 采集格式优先用目标端点真实引擎混音格式（float32 全链路位透明，无
  AUTOCONVERTPCM），不可用时回退 16bit / 44.1k + 引擎转换。

### 2. 自动端点静音（tap → redirect）

- 自动静音目标进程所在渲染端点的终点主音量，消除「原声 + 桥副本」双重声
  （ASIO 不走 WDM 终点音量，捕获点在终点音量之前，两者均不受影响）；
- 链路重建 / 退出时自动恢复音量；
- 诊断开关 `--no-mute`（跳过静音，观察双重声）。

### 3. 双环速率锁（时钟漂移校正）

- QPC 时间戳对 + 最小二乘测两时钟真实速率 → ratio 基值；
- 水位闭环微调；
- 暂停 / 恢复断档自动截断回归窗口（60 秒短窗、500ms 断档检测、15 秒清洁段要求），
  切歌后读数约 15 秒恢复（断档期间保持上一好值）。

### 4. 水位快排

水位超 2.5× 目标时直接丢弃陈旧预滚帧，秒级回落，避免长延时堆积。

### 5. DSP 链（全部实时可调，共享于所有输出后端）

| 模块 | 说明 | 参数 |
| --- | --- | --- |
| 分数重采样 | 速率锁输出 → 输出采样率 | 抽头数 `src_taps` |
| TPDF 抖动 | 降低量化底噪（默认开） | `dither` 开关 |
| 300B 电子管染色 | FET 平方律 + DC 阻断器，暖声 | 暖度 0–100% |
| 厚度与宽度 | 仅对谐波残差做 2–100ms 预延时（干信号不动），L/R 宽度偏移（0/5/12/25），HPF 160Hz −36dB/oct + LPF 8kHz −12dB/oct | 开关 / 延时 / 宽度档 |
| 火箭推进器 | 湿增益 0–18dB 只作用于延时谐波层，干信号不变 | 开关 / 增益（显示为 %） |

直通模式（`passthrough`）停用重采样（ratio 恒 1.0，逐位直通）。

### 6. 输出后端抽象（ASIO / WASAPI 自动切换）

- 统一接口：初始化、拉取回调、关闭、信息查询、崩溃检测、抖动开关；
- **设备扫描**：枚举 WASAPI 端点 ↔ ASIO 驱动，模糊名匹配 + 别名表
  （MADIface→RME/ADI-2、Realtek、Focusrite、Scarlett 等）；
- **按所选设备自动决定后端**：匹配到 ASIO 驱动走 ASIO，否则 WASAPI 独占；
- **启动自动适配（通用发行版，无需配置）**——未手动选择设备时按优先级自动选：
  ① 设备列表中匹配到 ASIO 的端点 → ② 注册表枚举到的第一个 ASIO 驱动
  （纯 ASIO 声卡无 WASAPI 端点时）→ ③ 第一个 WASAPI 独占端点；
  自动选中的 ASIO 初始化失败（驱动未装 / 设备未接）自动降级 WASAPI 独占；
- WASAPI 独占渲染：事件驱动线程，`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` 自动重试，
  float32 / int16 / int32 × 48k / 44.1k / 32k 候选格式探测；
- 运行中可通过 Web 控制台切换设备、重新扫描。

### 7. 内嵌 Web 控制台（http://127.0.0.1:3999）

复古金属仪表风：拉丝金属铭牌、12AX7 电子管指示灯、机械按键 / 拨杆、圆形启动按钮、
桥接主开关。功能面板：

- **实时仪表**：水位示波、速率锁读数（ratio 基值 / 输入输出采样率 / ppm）、
  频谱（干信号 + 染色残差叠加）、链路 LED 状态、总延迟、欠载计数；
- **设备面板**：输出设备下拉（含 ASIO 标记）+ 重新扫描按钮；
- **音色面板**：电子管开关 + 暖度滑块、厚度开关 + 延时滑块 + 宽度拨杆、
  火箭推进器开关 + 增益滑块（百分比显示）；
- **控制开关**：TPDF 抖动、直通模式、桥接主开关（实时启停链路）、
  重置统计 / 重建链路；
- **历史面板**：水位 / 速率历史曲线。

### 8. 崩溃自愈

- 线程级崩溃定位：`g_where` 线程局部化，崩溃日志带 tid / 线程身份 / 时间戳；
- 异常退出自动拉起新实例（`--respawn` 等待旧实例退出，避免互斥锁 / 静音恢复冲突）；
- 故意崩溃自检：`--crash-test` 验证崩溃 → 拉起全链路。

### 9. 一键安装与干净卸载

- **Inno Setup 一键安装包**（`packaging/asio_bridge.iss` 构建）：
  静态链接 MSVC 运行库，免装运行库；装到 `%LOCALAPPDATA%\ASIO Bridge`；
  自动建桌面 / 开始菜单快捷方式 + 开机自启计划任务（隐藏启动）；
- **一键干净卸载助手** `uninstall_helper.exe`（随安装包分发，双击即用）：
  确认框列出将执行的每项操作与检测实况 → 执行 → 逐项 ✓/–/✗ 结果反馈；
  停止桥进程 → 移除计划任务 → 删除快捷方式 → 删除安装目录（含自删）；
  内置安全护栏（仅允许删除名称含 “ASIO Bridge” 的非系统用户目录，防误删）；
  支持 `--silent` 静默模式（脚本 / 自动化）；
- 安装包内附《使用教程.txt》（13 章：快速上手 / 控制台 / 音色 / 设备切换 /
  自检 / 故障排查）。

### 10. 数值自检（先测后上线）
| 自检 | 验证内容 |
| --- | --- |
| `--resampler-test` | 重采样器正弦离线数值验证 |
| `--tube-test` | 300B 染色 + DC 阻断器离线验证 |
| `--pll-test` | 速率锁 + 水位闭环联合仿真 |
| `--capture-test` | 渲染 997Hz 正弦 → 按 PID 回环自采 → RMS / 峰值 / 频率比对 + 静音语义验证 |
| `--limiter-test` | 采集限幅器位置实测 |
| `--conv-test` | ASIO 格式转换离线验证 |

### 11. 在线升级（预留接口 + 完整链路）

- **版本检查**：启动时后台线程检查更新源，控制台顶部横幅显示
  「发现新版本 vX.X.X」；也可点「检查更新」手动触发；
- **更新源可配置**：`asio_bridge.cfg` 的 `update_url` 指向清单文件
  （支持国内服务器 / Gitee / 自建），未配置时默认 GitHub Releases API；
- **清单格式**（HTTP GET 返回，JSON 或 INI 均可）：
  `{"version":"1.0.1","url":"https://…/setup.exe","sha256":"<64hex>"}`
  （GitHub Releases API 的 JSON 自动适配其字段名）；
- **一键升级**：控制台点「下载并升级」→ 下载安装包 → SHA256 校验
  （清单提供时）→ 静默运行安装程序 → 桥自动退出；
- **网络失败静默**：检查/下载失败仅横幅提示，绝不影响音频主功能；
  出错 30 分钟重试，已是最新 6 小时重查一次。

---

## 构建

需要 VS2022（MSVC）+ CMake ≥ 3.20。ASIO SDK 2.3 已随仓库提供（见
`third_party/ASIOSDK2.3`，其许可为 Steinberg ASIO Licensing Agreement）。
静态链接 MSVC 运行库（/MT），产物为免依赖单文件 exe。

```powershell
cmake -B build -S .
cmake --build build --config Release
```

一键安装包：`"C:\Program Files\Inno Setup 7\ISCC.exe" packaging\asio_bridge.iss`
→ `packaging\output\asio_bridge_setup.exe`。

---

## 使用

```powershell
# 列出 ASIO 驱动
.\build\Release\asio_bridge.exe --list

# 测试 ASIO 链路（1kHz 正弦直出，自动选第一个可用 ASIO 驱动）
.\build\Release\asio_bridge.exe --tone --rate 44100

# 数值自检（渲染→回环自采比对，含静音语义）
.\build\Release\asio_bridge.exe --capture-test

# 正式桥接：自动发现并锁定最响的渲染进程
.\build\Release\asio_bridge.exe
```

完整命令行参数：

| 参数 | 说明 |
| --- | --- |
| `--list` | 列出 ASIO 驱动 |
| `--tone` | 1kHz 正弦直接经 ASIO 输出（验证 ASIO 链路） |
| `--driver <名字>` | 指定 ASIO 驱动（默认自动选择：设备匹配 ASIO → 注册表首个 → WASAPI 独占） |
| `--rate <Hz>` | `--tone` 模式采样率（默认 44100） |
| `--buffer <帧数>` | ASIO 缓冲帧数（默认驱动值，低延迟可试 128 / 64） |
| `--log <文件>` | 追加写入日志文件（后台 / 自启运行用） |
| `--dither` / `--no-dither` | 开关 TPDF 抖动（默认开） |
| `--passthrough` | 直通模式（ratio 恒 1.0，逐位直通） |
| `--no-mute` | 诊断：跳过端点静音 |
| `--resampler-test` / `--tube-test` / `--pll-test` / `--capture-test` / `--limiter-test` / `--conv-test` | 数值自检 |
| `--crash-test` | 故意崩溃，验证崩溃自愈链路 |
| `--respawn <毫秒>` | （内部）崩溃自愈拉起前等待旧实例退出 |

使用步骤：

1. 启动 Bridge 后播放任意应用音频（首次运行请确认该应用有输出设备可用）。
2. Bridge 10~20 秒内自动锁定该进程（控制台「目标进程」可见 PID 与活跃态）。
3. 控制台 `http://127.0.0.1:3999` 提供设备切换、音色调节、水位下限、TPDF 抖动、
   直通模式、重建链路、重置统计等控制。
4. 后台自启：`tools\install_autostart.ps1`（计划任务 + 隐藏启动，示例参数见脚本）；
   卸载用安装包内的 `uninstall_helper.exe`。

---

## 配置文件（asio_bridge.cfg）

与 exe 同目录，程序退出时保存、启动时加载：

| 键 | 说明 |
| --- | --- |
| `tube_on` / `tube_warmth` | 电子管染色开关 / 暖度 |
| `thickness_on` / `thickness_delay` / `thickness_width` | 厚度与宽度开关 / 延时 ms / 宽度档（0–3） |
| `booster_on` / `booster_db` | 火箭推进器开关 / 湿增益 dB |
| `passthrough` | 直通模式 |
| `floor` / `src_taps` | 水位下限 / 重采样抽头数 |
| `dither` | TPDF 抖动开关 |
| `update_url` | 在线升级清单地址（可选；空=内置 GitHub Releases） |

---

## 关键机制

- **取点位置**（`--capture-test` 阶段 4 实测）：回环取点在「会话静音之后、
  终点主音量之前」——因此用终点主音量静音做 redirect（会话静音会哑掉捕获）。
- **双环结构**：水位环（采集 → 缓冲 → 消费）与速率环（QPC 配对测频）解耦，
  消费端始终按输出时钟拉流。
- **设备匹配**：WASAPI 端点 ↔ ASIO 驱动通过模糊名匹配 + 别名表关联，
  保证「选设备 → 自动定后端」对用户透明。

---

## 已知限制

- 需要 Windows 11 22H2+（进程回环 API）。
- 端点静音作用域为整端点：该端点上其他 WDM 声音（系统提示音等）一并静音。
- 目标应用暂停时回环无数据包，短暂欠载由自适应水位吸收。
- RME MADIface ASIO 需要 STA 公寓模型（回调线程不得阻塞 COM）；
  设备扫描在独立 MTA 工作线程进行，避免 RPC_E_CHANGED_MODE。

---

## 许可

本项目代码以 MIT 许可发布（见 `LICENSE`）。
`third_party/ASIOSDK2.3` 为 Steinberg ASIO SDK，按其随附许可协议分发。
