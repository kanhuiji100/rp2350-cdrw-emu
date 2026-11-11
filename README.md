# RP2350 USB CD-RW 光驱模拟器

把一块 **RP2350 / Pico 2** 做成 USB 外置 CD-RW 光驱：主机把它识别为 **SCSI Peripheral Device Type = 5 的光驱**，而不是 U 盘。microSD 卡上的 `.ISO` 作为只读 CD-ROM，`.CDR` 作为可重写 CD-RW；配合 ImgBurn、Linux `cdrecord`/Kylin 完成刻录、擦除和读取。

## 定位

这是两个姊妹项目中的 **即插即用 / 低成本** 版本：

| 项目 | 定位 | 硬件 | 传输 | 介质 |
|---|---|---|---|---|
| [pi3b-cdrw-emu](https://github.com/kanhuiji100/pi3b-cdrw-emu) | 大容量、高性能、完整光驱 | Orange Pi 3B / RK3566 + Debian | USB 2.0 HS | CD-RW / DVD-RW / BD-RE |
| **rp2350-cdrw-emu**（本项目） | 即插即用、低成本、单板 | RP2350 / Pico 2 | USB 1.1 FS | CD-RW |

RP2350 版本无需主机守护进程，插上 USB 即用；代价是 USB 1.1 带宽和仅 CD-RW。
大容量 DVD/BD 与更高吞吐请使用 Orange Pi 版本。

设备带 SSD1306 128×64 OLED 和 EC11 旋转编码器，可以脱机选盘、弹出、新建空白盘和快速擦除。

> 当前版本：**v1.2.0**
>
> 已验证路径：**ImgBurn 刻录/Verify**、**Linux cdrecord / Kylin**。
>
> Windows 自带“刻录光盘”仍不可用；Windows 下请使用 ImgBurn。详见 [已知限制](#已知限制)。

## 功能状态

| 功能 | 状态 |
|---|---|
| USB Full-Speed 光驱枚举（PDT=5） | ✅ |
| ISO9660 只读 CD-ROM | ✅ |
| `.CDR` 可重写 CD-RW 镜像 | ✅ |
| ImgBurn 刻录 + Verify | ✅ |
| Linux `cdrecord` / Kylin 刻录和读取 | ✅ |
| `BLANK` / 快速擦除 | ✅ |
| RESERVE TRACK / CLOSE / TOC / Disc Info | ✅ |
| OLED 状态页、菜单、光盘浏览 | ✅ |
| EC11 编码器操作 | ✅ |
| MCU 端直接新建 700 MiB 空白镜像 | ✅ |
| MCU 端快速擦除（首尾各 1 MiB 清零） | ✅ |
| FAT32 / exFAT 挂载 | ✅ |
| SD SPI DMA | ✅ |
| 写路径流水线 | ✅ |
| Windows 资源管理器识别为 CD-RW、显示 4x | ✅ |
| Windows 自带 Live File System / Mastered | ❌ 实验未通过（协议行为已与 Orange Pi 版同步，待硬件复测） |

## 硬件

最小配置：

- Raspberry Pi Pico 2 / RP2350A 板
- microSD SPI 模块 + microSD 卡
- SSD1306 128×64 I²C OLED（`0x3C`）
- EC11 带按键旋转编码器
- USB 数据线、杜邦线、外壳

### 接线

| Pico 2 | 外设 | 说明 |
|---|---|---|
| GP2 | microSD **SCK** | SPI0 SCK |
| GP3 | microSD **MOSI** | SPI0 TX |
| GP4 | microSD **MISO** | SPI0 RX |
| GP5 | microSD **CS** | 手动片选 |
| GP6 | OLED SDA | I2C1 |
| GP7 | OLED SCL | I2C1 |
| GP8 | 编码器 A | 内部上拉 |
| GP9 | 编码器 B | 内部上拉 |
| GP10 | 编码器按键 | 内部上拉，按下接 GND |
| 3V3 | OLED / SD / 编码器 VCC | **只能用 3.3 V** |
| GND | 所有模块 GND | 共地 |
| GP25 | 板载 LED | 状态指示 |

> **GP2–GP5 不能按“CS、SCK、MOSI、MISO”顺序随意改。** RP2350 的 SPI0 硬件映射固定为 GP2=SCK、GP3=MOSI、GP4=MISO、GP5=CS。正确接法见上表。详细说明和 SD 模块供电注意见 [docs/wiring.md](docs/wiring.md)。

## 编译与烧录

### 依赖

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build git \
  gcc-arm-none-eabi libnewlib-arm-none-eabi \
  python3 xorriso dosfstools mtools
```

仓库已带 Pico SDK 2.1.1 和 TinyUSB。

### 构建

```bash
./tools/build.sh
# 生成 build/cdrw_emulator.uf2
# 同时复制到 dist/cdrw_emulator_v1.2.0.uf2
```

自定义板子/引脚：

```bash
cmake -S . -B build -G Ninja -DPICO_BOARD=pico2 \
  -DPICO_SDK_PATH="$PWD/third_party/pico-sdk"
cmake --build build
```

### 烧录

1. 按住 BOOTSEL 插入 Pico 2，出现 `RPI-RP2` 盘。
2. 把 UF2 拖入。
3. 自动重启，OLED 进入状态页。

## 准备 SD 卡和光盘

### SD 卡

支持：

- FAT32
- exFAT（**只挂载/读写，不在设备上格式化**）

不支持 NTFS。卡上可以是 MBR 或 GPT 分区。

首次启动会在根目录创建 `/discs`。

### 新建空白盘（无需 PC）

OLED 菜单：

```text
MENU -> New blank
```

会创建 `/discs/DISCxx.CDR`，预分配 700 MiB，并把首尾各 1 MiB 清零。等待提示完成即可。

### 快速擦除

```text
MENU -> Erase
```

会清零镜像首尾各 1 MiB，并重置 `blank / data_lba / session / track`。不是完整 700 MB 清零，所以几秒完成，但足以清掉旧的 ISO/UDF 关键结构。

### 用 PC 工具创建/导入

```bash
# 700 MiB 空白 .CDR + .meta
python3 tools/cdrw.py create /media/$USER/SDCARD/discs/DISC01.CDR --size-mb 700

# 把现有 ISO 导入 .CDR
python3 tools/cdrw.py import ./data.iso /media/$USER/SDCARD/discs/DISC01.CDR --size-mb 700

# 查看状态
python3 tools/cdrw.py info /media/$USER/SDCARD/discs/DISC01.CDR

# 文件夹 -> ISO
python3 tools/cdrw.py mkiso ./my_files /tmp/my_files.iso --volume-id MYDATA
```

镜像格式：

- `.CDR`：2048 字节/扇区的 Mode-1 用户数据，不含 2352 字节同步头/ECC
- `.CDR.meta`：48 字节 little-endian 元数据
- `.ISO`：只读 CD-ROM

## 使用

### Linux

设备出现为 `/dev/sr0`：

```bash
dd if=/dev/sr0 of=/tmp/disc.iso bs=2048 count=1000 status=progress
```

刻录：

```bash
cdrecord -v dev=/dev/sr0 driveropts=burnfree -eject image.iso
```

Kylin 的镜像刻录也可用。

### Windows

- 资源管理器会显示 **CD-RW 驱动器**
- 双击可出现“如何使用这张光盘”
- 速度显示 4x
- **但 Windows 自带 Live File System / Mastered 仍未打通**
- 请使用 **ImgBurn** 刻录和 Verify

Windows 支持分析见 [docs/roadmap.md](docs/roadmap.md)。

接手开发请先读 [AGENT.MD](AGENT.MD) 和 [TODO.MD](TODO.MD)。

### 菜单

- 转动：选择
- 短按：进入/确认
- 长按：返回
- `Info`：路径、容量、数据末尾、NWA、状态
- `New blank`：新建空白盘
- `Erase`：快速擦除
- `Eject/Reload`：虚拟弹出/加载

## 速度

当前基线：

- USB Full-Speed 12 Mbps
- SD SPI 12 MHz（实际约 10.7 MHz）
- SD SPI DMA
- 双缓冲读 + 写路径流水线
- 实测约 **读 500+ KB/s，写约 400 KB/s**
- 4x 刻录稳定

16 MHz 读、16 MHz 写在该 SDXC/模块上均不稳定；20 MHz 不可用。不要用 16/20 MHz 固件做日常刻录。

## 项目结构

```text
.
├── CMakeLists.txt
├── src/
│   ├── main.c              # 主循环、启动
│   ├── usb_cdrom.c         # 自写 BOT 光驱类（PDT=5）
│   ├── usb_descriptors.c   # USB 描述符
│   ├── scsi.c/.h           # MMC/SCSI 子集
│   ├── media.c/.h          # 光盘状态机、扇区读写、meta
│   ├── storage.c/.h        # FatFs 文件/目录/新建/擦除
│   ├── sd_spi.c/.h         # SPI SD + DMA
│   ├── oled.c/.h           # SSD1306
│   ├── encoder.c/.h        # EC11
│   ├── ui.c/.h             # 菜单/状态页
│   └── font5x7.c/.h
├── third_party/fatfs/      # FatFs R0.15（FAT32 + exFAT）
├── tools/cdrw.py           # 镜像创建/导入/擦除/ISO
├── tests/                  # SCSI / BOT / 工具 / FatFs 冒烟
├── docs/
└── dist/
```

## 测试

```bash
./tests/run.sh
```

包含：

1. SCSI 命令响应
2. USB BOT 状态机
3. 镜像工具
4. FatFs + MBR/FAT32 + `f_expand`

当前测试通过。真实 SD 卡时序、USB 枚举和刻录仍需上板验证。

## 已知限制

- 只模拟 CD 类介质，不模拟 DVD。
- 不支持 UDF packet writing；Windows Live File System 未打通。
- Windows 自带刻录不可用；请使用 ImgBurn。
- 不支持在资源管理器里删除单个文件；等价操作是擦除后重写。
- exFAT 只支持挂载/读写，不在 MCU 上格式化；卡需先在 PC 上格式化为 exFAT。
- 快速擦除只清首尾各 1 MiB，不是完整 700 MB 擦除。
- 4x 是 USB 1.1 下可靠速度；8x 以上可能 underrun。
- 16/20 MHz SD 时钟在该卡上不稳定，默认固定 12 MHz。
- OLED 固定 SSD1306 128×64 I²C `0x3C`；SH1106 需要改初始化。

## 文档

- [接线](docs/wiring.md)
- [架构](docs/architecture.md)
- [验证记录](docs/verification.md)
- [故障排查](docs/troubleshooting.md)
- [后续路线](docs/roadmap.md)
- [USB FS 提速分析](docs/usb-fs-tuning.md)
- [STM32H750 方案分析](docs/stm32h750-plan.md)

GitHub: <https://github.com/kanhuiji100/rp2350-cdrw-emu>

## 许可

MIT License。第三方组件：

- Pico SDK：BSD-3-Clause
- TinyUSB：MIT
- FatFs：FatFs 许可
- 5×7 字体：BSD/公共领域

详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
