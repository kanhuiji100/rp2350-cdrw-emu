# RP2350 CD-RW Emulator — 后续方案与开发计划

当前基线：**v1.1.0** 在 Linux/Kylin + cdrecord、ImgBurn 下已能刻录和 Verify。
Windows 10 仍能识别为 CD-RW、显示 4x、执行 FORMAT UNIT，但 IMAPI 不发 WRITE。

---

## 1. PIO USB HOST + FAT32 U 盘

### 可行性

可行。RP2350 的 PIO 可以实现 USB 1.1 Full-Speed Host，配合 TinyUSB Host MSC + FatFs 读取 U 盘的 FAT32 分区。RP2350 有 3 个 PIO、12 个状态机，PIO USB 只占 2 个 GPIO、1 个 SM 和少量 DMA，和现有 USB Device、SD SPI 可以共存。

需要额外处理：

- 5V VBUS 供电和限流/ESD；U 盘峰值 100–300 mA
- USB-A 母座、热插拔检测、去抖
- 只支持 FAT32；exFAT/NTFS 需要额外文件系统
- Host 和 Device 两套 TinyUSB 栈的资源/中断协调

### 性能

USB Full-Speed 理论 12 Mbps = 1.5 MB/s。MSC BOT + FAT32 的实际值通常是：

| 路径 | 典型顺序性能 |
|---|---|
| PIO USB Host 读 U 盘 | 0.8–1.1 MB/s |
| PIO USB Host 写 U 盘 | 0.5–0.8 MB/s |
| 当前 SD SPI 12 MHz 读 | 约 0.54 MB/s（实测） |
| 当前 SD SPI 12 MHz 写 | 约 0.35 MB/s（实测） |

如果镜像在 U 盘上，同时又要通过 USB Device 输出给 PC，会出现**双 USB Full-Speed 瓶颈**，端到端可能只有 0.4–0.6 MB/s，低于当前 SD 方案。

**结论：**

- 适合“没有 SD 卡也能用”的便利性方案
- 不适合作为提速方案
- 若要 24x/52x，USB Host 也必须是 High-Speed，RP2350 做不到

---

## 2. 在 MCU 上创建/擦除 CDR 镜像

### 快速创建

`.CDR` 本质是 2048 字节扇区的裸镜像 + `.CDR.meta`。MCU 可以：

- `f_open` 新建文件
- `f_expand` 预分配容量
- 写 meta：`blank=1, data_lba=0, capacity=...`

不需要真的把 700 MB 写零；读路径在 `blank` 或 `data_lba` 之后返回零即可。创建可以是**秒级**。

### 擦除

- **快速擦除**：只重置 meta，秒级；旧数据仍占物理块，但逻辑上不可见
- **完整擦除**：700 MB 写零，按 351 KB/s 约 33 分钟，不实用

当前“断电后还能看到旧 ImgBurn 数据”的根因就是快速擦除没有清数据区。正确做法是：

- 新建镜像而不是复用旧文件
- 或在快速擦除时同时清掉旧文件系统关键区域（前几 MB + 末尾）
- 或在镜像文件名中加入唯一 ID，避免旧 meta 复用

### 在 MCU 上生成 ISO/UDF

- **ISO9660**：可行。PVD、路径表、目录记录、文件数据，代码量可控； Joliet/Rock Ridge 再增加一些
- **UDF**：不建议在 MCU 上从零实现。UDF 2.01/2.50 的 anchor、partition、space bitmap、VAT、文件入口都复杂，且 Windows Live File System 对一致性很敏感

### 工具光盘方案（推荐）

比“MCU 生成 UDF”更实用：

- 构建时用 `genisoimage`/`xorriso` 生成一个只读工具 ISO
- 把 ISO 以 `const` 数组嵌入固件
- TinyUSB MSC 增加 **第二个 LUN**，只读、PDT=5
- 没有选择镜像时，LUN0 就是工具光盘；选择镜像后，LUN0 是 CD-RW，LUN1 是工具光盘

工具光盘内容：

- `cdrw.py`
- Windows/Linux 使用说明
- 驱动/固件版本
- 示例 ISO 制作脚本

Pico 2 有 4 MB Flash，当前固件约 137 KB；1–2 MB 的工具 ISO 完全放得下。需要注意链接脚本和 UF2 大小。

**建议优先级：工具光盘 > MCU 生成 UDF > MCU 完整擦除。**

---

## 3. 24x/52x 与 USB 2.0 方案

### 速度目标

| 倍速 | 数据率 |
|---|---|
| 4x | 0.6 MB/s |
| 24x | 3.6 MB/s |
| 52x | 7.8 MB/s |

CD-RW 实际写入通常 4x–10x；24x/52x 主要是 CD-R 读取或 CAV 写入。模拟器可以报告更高倍速，但存储和 USB 必须跟得上。

### RP2350 的限制

RP2350 原生 USB 只有 **Full-Speed 12 Mbps**。PIO 也不能产生 480 Mbps HS 信号。因此：

- 24x 需要 3.6 MB/s，超过 USB FS 实际上限
- 52x 需要 7.8 MB/s，更不可能
- 即使只读，USB FS 也会先成为瓶颈

### 便宜的 USB 2.0 High-Speed 方案

| 方案 | 成本 | 优点 | 缺点 |
|---|---|---|---|
| STM32H750/H723 WeAct 板 | 约 ¥70–150 | USB HS PHY 内置、SDMMC 4-bit、性能足够 24x | 需要移植固件、生态不如 Pico |
| STM32F723/F746 Discovery | ¥150–300 | HS PHY、资料多 | 板子较大、成本高 |
| i.MX RT1010/1020 | ¥100–200 | HS、性能强 | SDK 学习成本 |
| ESP32-P4 | 新平台 | USB HS、便宜 | 生态和 USB Device 光驱案例少 |
| Raspberry Pi Zero 2 W | ¥100–150 | Linux、USB OTG HS、工具链成熟 | 必须用 FunctionFS 自定义光驱，不是现成 g_mass_storage |
| Raspberry Pi 4/5 | ¥300+ | 性能强 | USB-C 口不是 Device，不能直接做 USB 光驱 |

**推荐：**

- 想做“产品化 24x”：**STM32H750/H723 + USB HS + SDMMC 4-bit**
- 想做“快速原型/带网络/带 UI”：**Pi Zero 2 W + FunctionFS**
- 不建议继续在 RP2350 上追 24x；瓶颈是 USB，不是 SD

### Linux 单板方案

优点：

- 文件系统、ISO/UDF 工具、网络、Web UI 都现成
- SDIO/eMMC 吞吐足够 24x
- USB OTG HS 可做 Device

缺点：

- **USB 光驱 gadget 不是标准功能**：`g_mass_storage` 的 `cdrom=1` 是只读，无法实现自定义 SCSI 写命令
- 需要 **FunctionFS** 自己实现 BOT/SCSI，工作量和 RP2350 方案相当
- 启动时间、断电文件系统损坏、功耗、体积都不如 MCU
- Pi Zero 2 W 的 OTG 和供电需要底板

**结论：**

- 24x/52x 必须换平台
- 最便宜且可控的是 STM32H7 系列
- Linux SBC 适合原型和带 UI 的产品，不适合“即插即用光驱”的低成本量产

---

## 4. 非标准容量 CD-R

可以。

容量由这些响应共同决定：

- `READ CAPACITY`
- `READ TOC` leadout
- `READ DISC INFORMATION`
- `READ TRACK INFORMATION`
- `READ FORMAT CAPACITIES`
- ATIP/lead-in（如果主机读取）

只要这些值一致，模拟器可以报告 650 MB、700 MB、800 MB、900 MB 甚至 99 分钟盘。限制：

- `.CDR` 文件必须足够大，或读越界时返回零
- MSF 地址最大 99:59:74
- Windows/ImgBurn 可能对非标准容量给出警告
- 部分主机可能按 ATIP 校正，需保持一致

建议：

- 默认 700 MiB / 358400 扇区
- 提供 650/800/900 MB 预设
- 非标准容量标记为 experimental

---

## 5. Windows 支持方案与开发计划

### 当前状态

- Windows 10 能识别为 CD-RW
- 显示 4x
- 能发 `FORMAT UNIT`
- 能读 `READ DISC STRUCTURE`、lead-in/out
- 但格式化后只轮询 TOC/Track Info，不发 WRITE / SEND CUE SHEET
- Linux/Kylin + cdrecord 已能刻录和读取

### 可能根因

1. IMAPI 的格式化完成事件/后台格式化状态没有满足
2. Live File System 的 UDF 写入条件没有满足
3. Mastered 路径缺少 `SEND CUE SHEET`/`RESERVE TRACK` 的某个响应
4. 媒体状态机（Empty/Incomplete/Complete/Appendable）与 IMAPI 预期不一致
5. Windows 对 USB 模拟 CD-RW 的格式完成判定依赖真实硬件时序

### 分阶段计划

#### Phase 0 — 冻结可用基线（1 天）

- 以 v1.0.51 为 Linux/ImgBurn 基线
- 回归 ImgBurn 刻录 + Verify
- 发布 `v1.0.51` 稳定包
- Windows 实验分支单独保留

#### Phase 1 — 抓包与对照（3–5 天）

- Windows 上用 **USBPcap + Wireshark** 抓：
  - 真实 USB CD-RW 的 Live File System 格式化
  - 真实 USB CD-RW 的 Mastered 刻录
  - 本模拟器的同样流程
- 打开 IMAPI ETW/WPR 日志
- 对比 `FORMAT UNIT` 后的所有命令、状态、事件

**产出：** 一张“真实光驱 vs 模拟器”的命令差异表。

#### Phase 2 — 先做 Mastered（1–2 周）

Mastered 不依赖 UDF，路径更短：

- 实现/修正 `RESERVE TRACK`
- 实现 `SEND CUE SHEET` 的真实解析或至少正确响应
- 确保 `WRITE(10/12)`、`SYNCHRONIZE CACHE`、`CLOSE TRACK/SESSION`
- 用 ImgBurn 和 Windows Mastered 双测

#### Phase 3 — 再做 Live File System（1–2 周）

- 修正 `FORMAT UNIT` 完成事件（`GET EVENT` 0x07 或 IMAPI 实际期望值）
- 校准 `READ DISC INFORMATION` 状态机
- 处理 UDF 写入的随机 LBA
- 必要时提供“预格式化 UDF 镜像”作为 fallback

#### Phase 4 — 工具光盘与容量（2–3 天）

- 第二 LUN 工具光盘
- 非标准容量预设
- 文档和测试脚本

#### Phase 5 — 测试矩阵（3–5 天）

- Windows 10/11
- ImgBurn
- Kylin/cdrecord
- Live File System / Mastered
- 700 MB / 800 MB / 900 MB

### 需要的外部条件

- 一台真实 USB CD-RW（用于抓包对照）
- USBPcap/Wireshark
- Windows 10/11 测试机或 VM
- 最好有 USB 协议分析仪；没有的话 USBPcap 也够用

### 风险

- 高：IMAPI 的行为可能依赖真实硬件时序，纯软件模拟未必能完全满足
- 中：UDF 写入的随机 LBA 与缓存一致性
- 低：Mastered 路径，已有 ImgBurn 验证基础

### 建议

- **不要再用真实 CD-R 做实验**
- 先把 v1.0.51 作为 Linux/ImgBurn 正式版
- Windows 支持作为独立实验分支推进
- 如果 Phase 1 抓包发现 IMAPI 依赖真实的 format completion 时序，优先实现事件和状态机，而不是继续猜字段

---

## 结论与优先级

1. **先发布 v1.0.51**：Linux/Kylin 已可用，这是目前最完整的基线
2. **工具光盘**：投入小、体验提升大，优先做
3. **USB Host**：可以做，但不要指望提速
4. **24x/52x**：换 STM32H7 或 Linux SBC，RP2350 不适合
5. **非标准容量**：可以做，保持各响应一致
6. **Windows**：按 Phase 1 抓包再动手，不要继续盲试
