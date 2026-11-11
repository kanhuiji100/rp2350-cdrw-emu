# 验证记录

> **当前状态（v1.1.0）**
>
> - RP2350 实物上已验证：USB 光驱枚举、SD 卡读写、ImgBurn 刻录/Verify、Linux/Kylin + cdrecord 刻录和读取。
> - exFAT、MCU 端新建空白盘、快速擦除已加入。
> - Windows 10 能识别 CD-RW 并显示 4x，但 Windows 自带 Live File System / Mastered 仍未打通；请使用 ImgBurn。
> - 当前发布文件：`dist/cdrw_emulator_v1.1.0.uf2`；基线备份：`dist/cdrw_emulator_v1.0.55.uf2`。
>
> 下面保留开发过程中的历史记录；早期“未连接实物”的描述仅适用于对应版本。

记录本仓库在开发环境中的实际验证结果。

## 环境

| 项 | 值 |
|---|---|
| 开发机 | LMDE 7 (Debian 13), x86_64 |
| 交叉编译器 | `arm-none-eabi-gcc 14.2.1 20241119` |
| CMake | 3.31.6 |
| Ninja | 1.12.1 |
| Pico SDK | 2.1.1（仓库内 `third_party/pico-sdk`） |
| TinyUSB | 0.18.0（SDK 子模块） |
| FatFs | R0.15 |
| 目标板 | `pico2`（RP2350A） |

## 固件构建

```bash
./tools/build.sh
```

结果：

- 构建成功，零编译警告。
- `build/cdrw_emulator.elf`：text 136464 B，data 0，bss 121612 B
- `build/cdrw_emulator.uf2`：约 247 KiB
- Release 副本：`dist/cdrw_emulator.uf2`

最终重新构建后请以 `sha256sum` 为准；本次记录时的哈希：

```
ebf2e9b21e91177635405e7bfeaf5bec02d8c1474b87d1757046d5c0fdaafa40  dist/cdrw_emulator.uf2
```

v1.0.31 撤回 20 MHz 写测试版：该卡/模块在 block 0 即写失败，且失败后卡无法通过 CMD0 复位。默认 12 MHz 写保持稳定。

v1.0.30 增加可选的 20 MHz 写时钟测试版；默认仍为 12 MHz。20 MHz 写失败会自动回退 12 MHz 并重新初始化卡。

v1.0.29 在 CMD25 前发送 ACMD23 预擦除块范围，并把块间 settle 从 200us 降到 100us，继续提升写入速度。

v1.0.28 修正 CMD25 每块 token：每个 block 前都发 0xFC，而不是只发首块；块间等待 busy。

v1.0.27 修正 CMD25 块间时序：每块数据响应后先给卡 200us 进入 busy，再等 MISO 释放；失败时发 0xFD 停止多块写、释放 CS 并重新初始化卡，再回退 CMD24。

v1.0.26 修正 CMD25 多块写协议：只发送一次 0xFC 起始 token，后续块不再发 0xFE，最后发 0xFD；失败时重新初始化卡并回退 CMD24。

v1.0.25 实现读双缓冲：两个 32 KiB 缓冲区，USB 发送当前块时预读下一块，SD/USB 重叠。

v1.0.24 移除 CMD25 多块写，恢复经过验证的 CMD24 单块写。部分 SDXC 卡拒绝 CMD25，失败后卡仍处于写模式，回退 CMD24 也会失败，导致 Writing LeadIn 卡死。

v1.0.23 将默认读取时钟恢复为 12 MHz：25 MHz 在部分 SDXC 卡/模块上会导致 `f_mount` 的首次读失败。CMD25 多块写保留。

v1.0.22 读操作切到 25 MHz SPI、写操作保持 12 MHz，并增加 CMD25 多块写（失败自动回退 CMD24），继续提升读/写吞吐。

v1.0.21 修复 SDSC 多块读每次固定约 100 ms 的问题：读完最后一个 block 后先等卡不忙，再发 CMD12，避免 `wait_r1()` 100 ms 超时；默认关闭逐条 READ 日志。

v1.0.18 增加 READ/SD 读取耗时日志和 25 MHz SPI 读取测试版，用于定位 Verify 读速瓶颈。

v1.0.16 补全 Full TOC（format 2）中缺失的 track 1 描述符。此前只返回 A0/A1/A2，ImgBurn 用 Full TOC 构建布局时会看到 0 个 track/session。

v1.0.15 实现真实的虚拟 eject/load：eject 卸载介质并产生 media change，load 从独立保存的路径重新打开同一镜像，强制 ImgBurn 在 Verify 前重新读取 TOC。

v1.0.14 在虚拟托盘 eject/load 时发送 Unit Attention（6/28）和 media event，强制主机刷新刻录前的 TOC 布局缓存，修复 ImgBurn Verify 的 `Session Count Not Equal (1/0)`。

v1.0.13 修正 finalize 后 Disc/Track/TOC 的 session 状态，避免 ImgBurn Verify 报 `Session Count Not Equal (1/0)`；并增加 DISCINFO/TOC 串口日志。

v1.0.12 把 SCSI `START STOP UNIT` 的 eject/load 改为虚拟托盘 no-op，避免 ImgBurn 验证前报 “unable to close tray”。

v1.0.11 改用 CMD24 逐块写（旧 SDSC 卡对 CMD25 多块写不兼容时会返回异常数据响应），修正 `0 bytes free` 日志顺序，并提供 8 MHz / 1 MHz SPI 版本。

v1.0.9 修复 SD 写数据块 CRC：此前每个 512 字节块发送固定的 `FF FF`，部分卡会以数据 CRC 错误拒绝写入，表现为 `f_write FR_DISK_ERR`。

v1.0.8 启用 UART0 调试日志（GP0 TX / GP1 RX，115200 8N1），记录 SD/FatFs/光盘状态和 SCSI CBW/CSW/WRITE 细节。

v1.0.7 在 SCSI 调试页显示写命令的 LBA/块数/块大小/sense，并对用户区之外的 lead-in/lead-out 写入做丢弃兼容；媒体写入失败现在返回 MEDIUM ERROR。

v1.0.6 增加 OLED SCSI 调试页（菜单 → SCSI debug），并在 `RESERVE TRACK` 后把盘状态转为 Incomplete，修复 ImgBurn `Writing LeadIn` 等待。

v1.0.5 修复了 ImgBurn “Getting MID” 兼容性：新增 ATIP/PMA 查询响应，修正 GET CONFIG 在 Unit Attention 期间失败，修正 ATIP/READ BUFFER CAPACITY 长度字段。

v1.0.3 针对空白 CD-RW 启用了 TAO 兼容模式：不再声明 Formattable/Restricted Overwrite，避免 Windows 尝试 UDF/Live File System 格式化；并修正空盘 Disc Information/Track Information 字段。

v1.0.2 修复了 SDSC（包括常见 2 GB 普通 MicroSD）不支持 CMD58 导致初始化失败的问题。

另有一个用于 SD 卡排查的 1 MHz SPI 版本：`dist/cdrw_emulator_sd1mhz.uf2`（仅用于确认低速下能否识别，正常刻录请用主固件）。

见 `dist/SHA256SUMS`。

## 主机测试

```bash
./tests/run.sh
```

实际输出摘要：

```
scsi tests: all passed
USB BOT tests: all passed
Ran 4 tests in 0.117s
OK
mounted, fat type 3, 127004 free clusters
  -       12 HELLO.TXT
hello.txt (12): hello world
f_expand 4MiB: 0 size=12
fatfs smoke: all passed
all host tests passed
```

覆盖内容：

### SCSI 单元测试（`tests/test_scsi.c`）

- INQUIRY：PDT=5、RMB=1、Vendor/Product 字段、VPD 0x00
- READ CAPACITY：last LBA、2048 字节块
- GET CONFIGURATION：当前 Profile 在 CD-RW/CD-ROM 之间切换、Profile List、Feature 列表
- READ DISC INFORMATION：blank/complete 状态、Erasable
- READ TOC：format 0 / 1 / 2、LBA/MSF
- READ TRACK INFORMATION：NWA、Free Blocks
- MODE SENSE 6/10：页 2Ah、读写速度字段
- WRITE 参数校验、写保护 sense
- Unit Attention + REQUEST SENSE
- GET EVENT STATUS NOTIFICATION
- READ FORMAT CAPACITIES

### USB BOT 状态机测试（`tests/test_usb_bot.c`）

在主机上编译真实的 `usb_cdrom.c`，用同步 shim 替代 TinyUSB 端点，验证：

- CBW 解析、方向、Tag、CDB
- INQUIRY 数据阶段 → CSW
- READ CAPACITY
- READ(10) 多扇区数据阶段
- WRITE(10) 数据阶段落盘 + CSW
- 写保护失败时先吸收 OUT 数据再返回 failed CSW
- Unit Attention → REQUEST SENSE → 恢复

该测试不经过真实 USB 控制器，但覆盖了固件类驱动的状态机与 SCSI 胶合层。

### 镜像工具测试（`tests/test_tool.py`）

- 创建空白 `.CDR`，检查大小、magic、CRC32、sidecar 回读
- ISO 导入：用户扇区布局、complete 标志、lead-out
- 擦除：sidecar 状态
- 损坏 CRC 被拒绝
- `mkiso`（有 xorriso 时）生成可识别的 ISO9660 PVD

### FatFs 冒烟测试

- 64 MiB 磁盘镜像，MBR 分区表，FAT32
- `FF_MULTI_PARTITION=1` 自动扫描分区
- 挂载、目录读取、文件读取
- `f_expand` 4 MiB 连续预分配
- 用于验证 `.CDR` 预分配和 SD 卡常见分区布局

## 未验证项（需要实物）

- RP2350 原生 USB 在 Windows/Linux/macOS 上的实际枚举
- Windows `cdrom.sys` / Linux `sr` 对 INQUIRY 与 MMC 响应的最终接受度
- microSD SPI 在 25 MHz 下的信号完整性
- SSD1306 / EC11 实际硬件
- ImgBurn、wodim 实际刻录完整流程
- USB 1.1 总线下的持续写入吞吐与 4x 速度稳定性
- 热插拔、断电恢复、SD 卡坏块行为

建议上板后依次执行：

```bash
lsusb -d 1209:cd01
dmesg | grep -Ei 'sr0|cdrom|scsi'
sudo sg_inq /dev/sr0
sudo sg_readcap -b /dev/sr0
sudo wodim -v dev=/dev/sr0 -speed=4 -data test.iso
```

## Windows IMAPI 兼容实验版 v1.0.51

- 文件：`dist/cdrw_emulator_winburn.uf2`
- 目标：让 Windows 资源管理器把空白 CD-RW 识别为可刻录光盘，并弹出“如何使用这张光盘”对话框。
- 新增能力：
  - `GET CONFIGURATION` 声明 Formattable、Restricted Overwrite、CD Mastering
  - `FORMAT UNIT`：支持带/不带参数表，格式化后进入 Incomplete/Appendable 状态
  - `SEND CUE SHEET`：接受并忽略 cue sheet 数据
  - `GET EVENT STATUS NOTIFICATION`：格式化完成事件 07h
- 该版本尚未替代稳定版；稳定版仍为 `dist/cdrw_emulator.uf2`（v1.0.31）。

v1.0.34 修复 `GET CONFIGURATION` 特性描述符偏移错误。
v1.0.35 将 Feature Descriptor 按 Feature Number 升序排列。
v1.0.36 增加 4x Write Speed Descriptor，并尝试用 capacity leadout 显示容量。
v1.0.37 空白盘恢复空 TOC，保持与 Disc Information 一致。
v1.0.38 `READ FORMAT CAPACITIES` 对空白盘返回 unformatted（type 1），Live File System 已能触发 FORMAT UNIT。
v1.0.39 同时返回 unformatted + formatted 两个 capacity descriptor，并记录 READ 失败 LBA。
v1.0.40 把负 LBA 的 CD lead-in 读取映射为零扇区。
v1.0.41 处理高位虚拟 LBA 的 lead-in/lead-out 读取。
v1.0.42 实现 `READ DISC STRUCTURE (0xAD)`。
v1.0.43 格式化后空白 track 的 TOC leadout 改为容量。
v1.0.44 FORMAT UNIT 后进入 Complete 状态，并打印 format descriptor。
v1.0.45 格式化后的空白盘在 `READ TRACK INFORMATION` 中报告完整 free blocks。
v1.0.46 Track Information 的 blank 标志改为由 `data_lba == 0` 决定。
v1.0.47 增加 `TRACKINFO` 36 字节日志；字段确认正常：track=1/session=1/mode=4/blank=1/NWA_V=1/free=358400。
v1.0.48 FORMAT UNIT 后回到 Empty/Blank 状态。
v1.0.49 实现 `READ BUFFER (0x3C)`。
v1.0.50 尝试实现 Ricoh Mode Page 0x30。
v1.0.51 修正 page 0x30 被误加到 INQUIRY、而非 MODE SENSE 的问题；`MODE SENSE 5A 00 30` 现在返回 Burn-Free 页。

SHA256：

```text
01ffb0a641bc9309fcaf096de980c1a37e021caf58a5a7ddca2e262f5e5500f0  dist/cdrw_emulator_winburn.uf2
```

## Windows 自带刻录实验结论（v1.0.48）

- 设备已被 Windows 10 识别为 CD-RW，刻录向导可见，速度可选 4x。
- Live File System 能触发 `BLANK`、`MODE SELECT`、`FORMAT UNIT`。
- 已实现并对齐：`GET CONFIGURATION` 特性、`READ FORMAT CAPACITIES`、`READ TOC`、`READ DISC INFORMATION`、`READ TRACK INFORMATION`、`READ DISC STRUCTURE`、lead-in/lead-out 虚拟 LBA。
- 格式化后无论返回 Empty、Incomplete 还是 Complete，Windows 均只轮询 TOC/Track Info，不发送 WRITE / SEND CUE SHEET。
- 结论：Windows IMAPI 对 USB 模拟 CD-RW 的格式完成判定仍未满足；继续需要 USB/SCSI 总线抓包或真实光驱同流程对比。
- 可靠刻录路径仍然是 ImgBurn。稳定固件为 `dist/cdrw_emulator.uf2` v1.0.31。

## Kylin / cdrecord 测试结论

- v1.0.49 后 `READ BUFFER (3C)` 已不再失败。
- cdrecord 仍停在 50%，日志中没有 WRITE / RESERVE TRACK。
- Kylin 使用 `cdrecord ... -multi`；内核提示 `Hmm, seems the drive doesn't support multisession CDs`。
- 下一步：在 Kylin 关闭“多区段/稍后添加文件”，或手动去掉 `-multi`，验证单区段刻录。
- Windows 自带 IMAPI 仍不发 WRITE，与 Linux 这条路径独立。

## v1.0.52 读写提速实验

- 写路径改为流水线：USB 接收下一块的同时，SD 写当前块。
- 读路径保持双缓冲。
- 读时钟 20 MHz 作为独立测试固件；f_mount 失败会自动回退到 12 MHz。
- `dist/cdrw_emulator_winburn.uf2`：12 MHz 读 + 写流水线。
- `dist/cdrw_emulator_r20.uf2`：20 MHz 读 + 写流水线。
- 预期：写从约 351 KB/s 提升到 500–650 KB/s；读 20 MHz 若稳定，可到 800–1000 KB/s。

## v1.0.53 读写提速实验二

- 修复 20 MHz 回退时只降时钟、不重新初始化卡的问题；现在会 `sd_init()` 后重试 mount。
- `dist/cdrw_emulator_winburn.uf2`：12 MHz 读 + 写流水线。
- `dist/cdrw_emulator_r16.uf2`：16 MHz 读 + 12 MHz 写。
- `dist/cdrw_emulator_r16w16.uf2`：16 MHz 读 + 16 MHz 写（写失败会自动回退 12 MHz）。
- 20 MHz 已确认在该 SDXC 上不稳，不再提供。

## v1.0.54 SD 恢复与写 16 MHz 测试

- 15 MHz 读在该 SDXC 上 f_mount 失败；此前回退后卡未恢复，导致 BLANK 报 Write Error。
- v1.0.54 的 `sd_init()` 对 CMD0 做 3 次重试（CS 高、补时钟、50 ms 间隔），可在失败读后重新拉回 SPI 模式。
- `dist/cdrw_emulator_winburn.uf2`：12 MHz 读 + 写流水线 + 恢复重试。
- `dist/cdrw_emulator_w16.uf2`：12 MHz 读 + 16 MHz 写；CMD25 失败自动回退 12 MHz。
- 16 MHz 读版本已移除；该卡/模块在 15 MHz 实际时钟下无法 mount。

## v1.0.55 SD SPI DMA

- 512 字节 SD 数据块改用 SPI0 TX/RX DMA。
- CMD18 读、CMD24/CMD25 写的数据阶段都走 DMA；token、CRC、busy 仍由 CPU 处理。
- `dist/cdrw_emulator_winburn.uf2`：12 MHz 读 + 12 MHz 写 + DMA + 写流水线。
- `dist/cdrw_emulator_w16.uf2`：12 MHz 读 + 16 MHz 写 + DMA。
- 需要验证读写数据完整性；DMA 只改变搬运方式，不改变协议。

## v1.1.0 exFAT + MCU 镜像生命周期

- FatFs 开启 exFAT（`FF_FS_EXFAT=1`），代码页改为 936（简体中文）。
- 只支持挂载/读写已格式化的 exFAT；FatFs 不负责把卡格式化成 exFAT。
- 新建镜像：`f_expand` 预分配后，把首尾各 1 MiB 清零，清除旧的 ISO/UDF 签名。
- 快速擦除：清零首尾各 1 MiB + 重置 meta；约 2 MiB 写入，不再只改 meta。
- 菜单已有 `New blank` / `Erase`，无需 PC 工具。
- `dist/cdrw_emulator_v1055.uf2` 保留为 v1.0.55 基线。
