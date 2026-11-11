# STM32H750VBT6 方案详细分析

## 1. 芯片与开发板规格

### STM32H750VBT6

| 项目 | 规格 |
|---|---|
| 内核 | Cortex-M7，480 MHz |
| 内部 Flash | **128 KB** |
| RAM | 1 MB（AXI SRAM + 其他 SRAM） |
| 封装 | LQFP100 |
| USB | USB OTG_HS（ULPI）+ USB OTG_FS |
| SD | SDMMC1 / SDMMC2，支持 4-bit |
| 外部存储 | QSPI / OctoSPI |
| 电压 | 3.3 V |
| 典型价格 | 芯片约 ¥30–50 |

关键点：**H750 的 USB High-Speed 需要外部 ULPI PHY**。芯片内部只有 Full-Speed PHY；HS 的 480 Mbps 必须通过 ULPI 接 USB3300/USB3320 等 PHY。

### 常见 ¥63 开发板

典型配置：

- STM32H750VBT6
- Type-C 接口
- TF 卡槽
- W25Q64（8 MB QSPI Flash）
- SWD 调试口
- 3.3 V LDO
- 可能带 CH340 / BOOT 按键 / LED

这类板子通常是“核心板”定位，**Type-C 多半只接了 USB OTG_FS（PA11/PA12），并没有 USB3300**。购买前必须确认原理图。

---

## 2. 决定性前提：USB High-Speed PHY

### 如果板子只有 Type-C 的 FS

- USB 12 Mbps，理论 1.5 MB/s
- BOT MSC 实际约 0.7–1.1 MB/s
- **不可能达到 24x（3.6 MB/s）**
- 换 H750 只会让 SD/CPU 更快，USB 仍是瓶颈
- 相比 RP2350 提升有限，开发成本却更高

### 如果板子有 ULPI 排针，或能外接 USB3300

- USB HS 480 Mbps
- BOT MSC 实际约 25–40 MB/s
- 24x / 52x 的 USB 瓶颈消失
- 还需要 SDMMC 4-bit 和足够快的 TF 卡

**结论：这块 ¥63 板子值不值得买，90% 取决于它是否引出 ULPI。**

---

## 3. 购买前检查清单

让卖家提供原理图，重点确认：

1. **Type-C 的 D+/D- 接到哪里**
   - PA11/PA12 = USB OTG_FS，只有 12 Mbps
   - 是否有 ULPI 的 12 根信号（见下）

2. **ULPI 信号是否引出**
   - D0–D7
   - CLK（60 MHz）
   - STP
   - DIR
   - NXT
   - RESET
   - 3.3 V / GND

3. **TF 卡槽是 SDMMC 4-bit 还是 SPI**
   - 4-bit：PC8–PC12 + PD2（SDMMC1）
   - SPI：速度会掉到 1–2 MB/s，不适合 24x

4. **W25Q64 的 QSPI 接线**
   - 是否支持 memory-mapped XIP
   - 是否有 BOOT 切换
   - 是否有板载编程器/DFU

5. **SWD 是否引出**
   - 需要 ST-Link 烧录和调试

6. **3.3 V 电源余量**
   - H750 480 MHz + USB PHY + TF 卡，峰值可能 300–500 mA
   - Type-C 5V 输入是否足够

7. **ULPI 引脚是否与 QSPI/SDMMC 冲突**
   - H750 LQFP100 引脚复用很密
   - 需要逐脚核对

---

## 4. 性能估算

### 有 USB3300 / ULPI 的情况

| 项目 | 估算 |
|---|---|
| USB HS BOT | 25–40 MB/s |
| SDMMC 4-bit 25 MHz | 12.5 MB/s |
| SDMMC 4-bit 50 MHz | 25 MB/s |
| TF 卡顺序读 | 20–90 MB/s（取决于卡） |
| TF 卡顺序写 | 10–40 MB/s（好卡） |
| 24x CD | 3.6 MB/s |
| 52x CD | 7.8 MB/s |
| CD-RW 实际写入 | 4x–10x |

结论：

- **24x 读取/写入**：有希望
- **52x 读取**：USB 和 SDMMC 都够，CPU 也够
- **52x 写入**：CD-RW 本身不支持；CD-R 模拟可以报告，但实际受 TF 卡写入限制
- **CD-RW 4x–10x**：轻松

### 只有 USB FS 的情况

| 项目 | 估算 |
|---|---|
| USB FS BOT | 0.7–1.1 MB/s |
| 4x CD | 0.6 MB/s，勉强 |
| 8x CD | 1.2 MB/s，超过 USB FS 实际值 |
| 24x | 不可能 |

---

## 5. 软件移植工作量

现有 RP2350 代码可以复用大部分协议层：

| 模块 | 复用程度 |
|---|---|
| `scsi.c` | 高，主要改时间/日志/GPIO |
| `media.c` | 高，FatFs API 基本不变 |
| `usb_cdrom.c` | 中，TinyUSB 设备 API 类似，但 H7 需要 ULPI 和 cache 处理 |
| `sd_spi.c` | 低，改成 SDMMC 4-bit + DMA |
| `ui.c` / OLED / 编码器 | 中，改 I2C/GPIO |
| `main.c` | 低，重写时钟、初始化、USB 中断 |

### 需要新增/重点处理

1. **QSPI XIP**
   - H750 内部只有 128 KB Flash，当前固件约 137 KB，已经放不下
   - 需要 bootloader + W25Q64 XIP，或把部分代码放 RAM
   - 链接脚本、向量表、QSPI 初始化都要处理

2. **USB HS ULPI**
   - 60 MHz ULPI clock
   - USB3300 供电、复位、VBUS
   - TinyUSB H7 端口

3. **SDMMC + DMA + Cache**
   - H7 有 D-Cache，USB/SD DMA 前后要 clean/invalidate
   - 这是 H7 上最容易出数据损坏的地方

4. **工具光盘**
   - W25Q64 8 MB 可以放固件 + 1–2 MB 工具 ISO
   - 第二 LUN 只读

5. **Windows 协议**
   - 换平台不会自动解决 IMAPI 问题
   - Windows 支持仍需按 roadmap 抓包和修状态机

### 工作量估算

| 阶段 | 时间 |
|---|---|
| 板级 bring-up（时钟、QSPI、SDMMC、USB FS） | 3–5 天 |
| USB HS ULPI + TinyUSB | 3–7 天 |
| SDMMC 4-bit + FatFs + DMA/cache | 3–5 天 |
| 协议层移植 | 5–7 天 |
| OLED/编码器/外壳 | 2–4 天 |
| 性能调优与稳定性 | 1–2 周 |
| **合计** | **4–6 周** |

---

## 6. 成本估算

| 项目 | 价格 |
|---|---|
| H750VBT6 开发板 | ¥63 |
| USB3300 ULPI 模块 | ¥10–20 |
| TF 卡 | ¥15–30 |
| 杜邦线/转接板 | ¥10 |
| 外壳/接口 | ¥20–40 |
| **合计** | **约 ¥120–160** |

如果板子没有 ULPI，需要自己飞线 12 根信号，稳定性和信号完整性都会变差。

---

## 7. 替代方案对比

| 方案 | USB HS | 成本 | 难度 | 评价 |
|---|---|---|---|---|
| H750 + USB3300 | 是（外接） | ¥120–160 | 中高 | 性能强，但要飞线/核对引脚 |
| STM32F723/F730 | **是（内置 HS PHY）** | ¥100–200 | 中 | 无需 ULPI，推荐优先看 |
| STM32H723/H725 | 是（ULPI） | ¥100–200 | 中高 | 性能更强，仍需 PHY |
| STM32F407 + USB3300 | 是（外接） | ¥80–120 | 中 | 老平台，性能较低 |
| Pi Zero 2 W | 是（OTG） | ¥100–150 | 高（FunctionFS） | 软件自由度高，但启动/供电复杂 |
| RP2350 | 否 | 已有 | 低 | 只能 4x–8x |

**如果目标是 24x，优先考虑 STM32F723/F730**：内置 HS PHY，不需要 USB3300，硬件简单很多。

---

## 8. 推荐决策

### 推荐买这块 ¥63 板子的条件

- 原理图确认 **ULPI 信号引出**
- TF 卡槽是 **SDMMC 4-bit**
- W25Q64 支持 **QSPI XIP**
- 你愿意做 QSPI bootloader 和 USB3300 接线

### 不推荐买的情况

- Type-C 只接 PA11/PA12（FS）
- TF 卡槽是 SPI
- 没有 ULPI 排针，也没有把握飞线
- 只想“换块板子就自动 24x”

### 更稳的路线

1. **STM32F723/F730 开发板**：内置 HS PHY，少 12 根线
2. **H750 + USB3300**：性能更强，但硬件工作更多
3. **Pi Zero 2 W**：适合原型，不适合低成本即插即用产品

---

## 9. 建议的下一步

1. 向卖家要 **原理图/引脚图**
2. 确认 **ULPI、SDMMC 4-bit、QSPI**
3. 如果只有 FS，放弃这块板用于 24x，或只作为 **SDMMC + 工具盘** 验证平台
4. 如果 ULPI 可用，先做 **USB HS 枚举 + MSC 读写测试**
5. 再移植 SCSI/媒体层
6. Windows 支持仍然单独按 roadmap 推进

---

## 10. 结论

- **¥63 的 H750VBT6 板子性价比高，但默认很可能只有 USB FS**
- **没有 ULPI/HS PHY，就无法实现 24x，换平台意义不大**
- 有 ULPI 时，H750 + USB3300 可以做到 24x/52x 读取，SDMMC 4-bit 也够
- 软件复用度高，但 QSPI XIP、DMA cache、USB HS 是主要风险
- 若想少走硬件弯路，**STM32F723/F730 内置 HS PHY 更合适**
- 换平台不会自动解决 Windows IMAPI 问题；Linux/ImgBurn 仍是当前可靠路径

---

## 11. LXB750VB-P1 原理图实测结论

根据 `LXB750VB-P1原理图.pdf`：

### USB

```text
PA11 -> USB_D-
PA12 -> USB_D+
Type-C 直接连接到 STM32 的 PA11/PA12
```

原理图注释明确写着：

> TypeC 直接连接到 STM32 的 PA11 和 PA12，可开发 USB 应用

这是 **USB OTG_FS，12 Mbps**。

板上**没有 USB3300/USB3320**，也**没有 ULPI 排针**。

### TF 卡

```text
PC8  SDMMC1_D0
PC9  SDMMC1_D1
PC10 SDMMC1_D2
PC11 SDMMC1_D3
PC12 SDMMC1_CLK
PD2  SDMMC1_CMD
```

确认是 **SDMMC1 4-bit**，这点很好，存储侧足够 24x。

### W25Q64

```text
PB2  QSPI_CLK
PB6  OSPI_BK1_NCS
PD11 QSPI_BK1_IO0
PD12 OSPI_BK1_IO1
PE2  OSPI_BK1_IO2
PD13 OSPI_BK1_IO3
```

确认是 **QSPI W25Q64，8 MB**。

### 关键限制

- H750 内部 Flash 只有 **128 KB**
- 当前固件约 **137 KB**，已经超过内部 Flash
- 必须做 **QSPI XIP + bootloader**，或把代码搬 RAM
- 板上没有外部 SDRAM，只有 1 MB 内部 RAM

### ULPI 能不能飞线

理论上 H750 有 ULPI：

```text
PA3  D0
PA5  CLK
PB0  D1
PB1  D2
PB5  D3
PB10 D4
PB11 D5
PB12 D6
PB13 D7
PC0  STP
PC2  DIR
PC3  NXT
```

但这张板：

- **PA3、PA5 没有引到排针**，需要直接焊 LQFP100 引脚
- PB10–PB13、PB0/PB1/PB5 在排针上
- PC0/PC2/PC3 在液晶排针上
- 60 MHz ULPI 时钟走杜邦线，信号完整性很差
- 需要自己转接 USB3300 模块

**不推荐在这块板上飞线 ULPI。**

### 最终判断

| 目标 | 是否适合 |
|---|---|
| 4x CD-RW / 4x–8x CD | 可以，但 USB FS 仍然是瓶颈 |
| 24x CD | **不适合**（USB FS 12 Mbps） |
| 52x CD | **不适合** |
| SDMMC 4-bit 存储 | 适合 |
| W25Q64 + 工具光盘 | 适合，但需要 QSPI XIP |
| 作为 H7 学习板 | 适合 |
| 作为 24x 光驱主控 | **不适合** |

**结论：这块 ¥63 板子不能直接做 24x/52x。它的 USB 只有 FS，且没有 ULPI/HS PHY。**

如果目标只是 4x–8x，它可以用，但相比 RP2350 提升主要来自 SDMMC 和 CPU，USB 仍然限制在 1 MB/s 左右，性价比不高。

如果目标是 24x，优先选：

- **STM32F723/F730**：内置 HS PHY，不需要 ULPI
- **带 ULPI 排针的 H750/H723 板 + USB3300**
- **Pi Zero 2 W + FunctionFS**（原型）
