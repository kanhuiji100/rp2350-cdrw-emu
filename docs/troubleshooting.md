# 故障排查

## 1. 电脑完全看不到设备

现象：`lsusb` 没有 `1209:cd01`；Windows 设备管理器没有新设备。

检查：

```bash
lsusb | grep -i 1209
dmesg | tail -30
```

- 换一根**带数据**的 USB 线。
- Pico 2 是否进入了 UF2 刷写模式（BOOTSEL）而没有正常启动？拔掉重插，不按 BOOTSEL。
- 固件是否真的烧进去了？重新拖入 `build/cdrw_emulator.uf2`，或直接使用 `dist/cdrw_emulator_v1.1.0.uf2`。
- 目标板是否与编译目标一致？默认是 `pico2`。使用自定义 RP2350 板时要设置正确的 `PICO_BOARD`。
- USB 口供电不足：换主机后置 USB 口或带供电 Hub。
- 测量 3V3 是否稳定；SD 卡初始化电流峰值可能导致复位。

## 2. 能看到 USB 设备，但系统没出现光驱

```bash
lsusb -v -d 1209:cd01 | head -80
lsblk
dmesg | grep -Ei 'sr0|cdrom|scsi|usb-storage'
```

期望：

- `bInterfaceClass 8 Mass Storage`
- `bInterfaceSubClass 2`
- `bInterfaceProtocol 80`（十六进制 `0x50`）
- 内核出现 `sr 0:0:0:0: ...` 和 `/dev/sr0`

如果显示为 `/dev/sdX` 或“可移动磁盘”：

- 固件未使用自写类驱动，或 `INQUIRY` 没有返回 PDT=5。
- 确认链接的是本仓库的 `usb_cdrom.c`，而不是 TinyUSB 自带的 `msc_device`；`src/tusb_config.h` 中 `CFG_TUD_MSC` 必须为 0。
- 用 `sg_inq /dev/sr0` 查看外设类型；如果设备节点是 `/dev/sgN`，可先运行：

```bash
sudo sg_inq /dev/sg0
```

Windows 下：

```powershell
Get-CimInstance Win32_CDROMDrive | Format-List *
```

应该看到 `RP2350 CD-RW Drive` 之类的光驱条目，而不是 USB 磁盘。

## 3. 光驱出现了，但显示“无光盘”

这通常不是故障，而是**还没有插入虚拟光盘**：

- OLED 菜单 → `Discs`，选择 `.ISO` 或 `.CDR`。
- SD 卡目录里必须有文件，扫描范围是根目录和 `/discs`。
- 文件名/扩展名必须为 `.iso` 或 `.cdr`（大小写不敏感）。
- 如果 `NO MICROSD`，先解决第 4 节。
- 重新选择后，主机会收到 Unit Attention / Media Event；如果系统没刷新，执行：

```bash
sudo eject /dev/sr0
sudo eject -t /dev/sr0
# 或重新插拔 USB
```

设备菜单中的 `Reload` 会重新加载上次选择的光盘。

## 4. `SD INIT FAIL` / `CARD NOT FAT32` / 卡挂载失败

屏幕现在会区分两类错误：

- `SD INIT FAIL`：SPI/SD 卡没有完成初始化，优先查供电、接线、SPI 速度和卡座。
- `CARD NOT FAT32`：SD 卡已被识别，但 FatFs 找不到 FAT32 分区，优先查分区表和文件系统。

固件每隔 5 秒会自动重试，因此插卡后不必重启；但第一次上电时卡最好已经在位。

- **2 GB 普通 MicroSD 通常是 SDSC，不是 SDHC。** v1.0.2 已修复 SDSC 不支持 `CMD58` 时的初始化失败；如果还是 `SD INIT FAIL`，请确认刷入的是 v1.0.2 或更新版本。
- 卡必须格式化为 **FAT32**。Windows 对大于 32 GB 的卡可能只提供 exFAT；请使用 `fat32format`/`gparted` 等工具。
- 支持 MBR 和 GPT 分区表，也支持无分区表的 superfloppy。
- 不要使用 exFAT、NTFS、ext4。
- 检查模块供电是否为 3.3 V，CS/SCK/MOSI/MISO 是否接对。
- 杜邦线太长/太差会导致 25 MHz 下初始化失败：主固件默认已降到 12 MHz。仍失败时可直接烧录 `dist/cdrw_emulator_sd1mhz.uf2`（SPI 1 MHz）测试；如果这个版本能识别，说明是信号完整性/速度问题。
- 卡座接触不良：换卡、清洁触点。
- 可选卡检测：默认关闭；若启用 GP14，卡座 CD 应插卡时对地。

## 5. OLED 不亮

- 确认模块为 **SSD1306**，不是 SH1106。
- 地址通常是 `0x3C`，少数 `0x3D`；修改 `src/board.h` 的 `CDRW_OLED_ADDR`。
- SDA=GP6，SCL=GP7，VCC=3V3，GND 共地。
- 模块上一般有上拉；裸屏需要外部 4.7 kΩ 上拉。
- 若 OLED 缺失，USB 光驱功能仍应正常工作，只是没有显示。

## 6. 编码器不灵/方向反/按一次跳两格

- A/B 对调可反转方向。
- 编码器机械抖动可能导致一次跳两格：修改 `src/encoder.c` 的 `CDRW_ENCODER_TRANSITIONS_PER_DETENT`（默认 4，有的编码器是 2）。
- 按键接 GP10 与 GND，使用内部上拉。
- 如果旋转时 OLED 完全无反应，检查 A/B 是否接到 GP8/GP9，公共端是否接 GND。

## 7. 刻录软件找不到可写光驱

- 当前插入的必须是 `.CDR` 可写镜像；`.ISO` 是只读 CD-ROM。
- OLED 上看到 `CD-RW` 与 `BLANK`/`READY` 才表示可写盘。
- Windows 使用 ImgBurn/CDBurnerXP；资源管理器本身不支持向 CD-RW 拖放写入。
- Linux：`wodim --devices`，`dev=/dev/sr0`。

## 8. 刻录开始后失败 / Buffer Underrun / 写入错误

- **速度限制在 4x**。USB 1.1 的持续带宽支撑不了 8x 数据 CD，即使主机允许选择 8x。
- 使用 **TAO（Track At Once）**，不要选 SAO/DAO/RAW。固件没有实现完整的 Session-at-Once 和 RAW 写。
- 确保 SD 卡有足够空间；700 MiB 盘 + 元数据 + FAT 需要约 720 MiB 空闲。
- 用 `tools/cdrw.py create` 创建并预分配镜像；不要手动创建一个随机大小的文件当光盘。
- 检查 microSD 是否降速或坏块；换卡测试。
- 若之前失败过，先执行 `Erase`（或 ImgBurn 擦除），再重新刻录。
- 刻录过程中不要拔卡、重启或操作编码器新建盘。

## 8.1 Windows 空白 CD-RW 导致 BurnAware/资源管理器卡住

- 先确认 OLED 上 CDR 是 `BLANK` 还是 `READY`。`BLANK` 才能用于刻录。
- v1.0.3 起不再声明 `Formattable` 和 `Restricted Overwrite`，避免 Windows 尝试 UDF/Live File System 格式化；v1.0.5 起补上 ATIP/PMA 查询，ImgBurn 不再卡在 `Getting MID`。请先升级到 v1.0.5。
- 用 **ImgBurn** 交叉验证：如果 ImgBurn 能列出驱动器并开始刻录，而 BurnAware 卡在“检测设备”，是 BurnAware 兼容性问题。
- 如果 ISO 能挂载，但导入过 `demo.iso` 的 `.CDR` 也卡住，记录 OLED 显示的状态并反馈；这属于可写媒体状态机问题。
- 如果 OLED 在 Windows 卡住时仍能旋转/响应，说明固件主循环没有死；进入 `菜单 → SCSI debug` 查看最后几条命令。最后一条状态为 `--` 的命令就是主机正在等的命令。
- v1.0.6 起，`RESERVE TRACK` 后盘状态会从 Empty 变成 Incomplete，ImgBurn 不应再卡在 `Writing LeadIn`。
- v1.0.7 起，`SCSI debug` 页会显示最后一条写命令的 `L`(LBA)、`B`(块数)、`S`(源块大小)、`K/A/Q`(sense key/ASC/ASCQ)；`D` 表示该写被丢弃兼容（lead-in/lead-out）。

## 8.2 复制串口日志

- UART0：GP0（TX，物理 1 脚）接 USB-UART 的 RX，GND 共地，115200 8N1。
- 日志包含 SD 初始化、FatFs、光盘状态、SCSI CBW/CSW、WRITE 的 LBA/块数/sense。
- 打开串口助手后复位 Pico，日志从启动开始。
- 如果出现 `SD: CMD0 failed`、`f_mount failed`，先解决 SD 问题。
- 如果出现 `media: f_write failed ... FR=1`，这是 SD 卡写块错误，不是 SCSI 参数错误。
- v1.0.9 起写数据块使用正确 CRC16；v1.0.11 起改为 CMD24 单块写，兼容旧 SDSC 卡。
- 若日志出现 `SD: write data rejected ... resp=F2`，优先换 8 MHz 固件（`dist/cdrw_emulator_sd8mhz.uf2`）；仍失败再试 1 MHz。`resp=0B` 才是数据 CRC 错误。
- `storage: ... 0 bytes free` 在 v1.0.10 及更早是日志顺序 bug；v1.0.11 起显示真实空闲空间。

## 8.3 ImgBurn 刻录完成后提示无法关闭托盘

- 这是虚拟光驱没有物理托盘导致的，不是刻录失败。
- v1.0.12 起 `START STOP UNIT` 的 eject/load 作为虚拟托盘 no-op 处理，两个方向都返回成功，ImgBurn 会直接进入 Verify。
- 如果想真正卸载当前镜像，用 OLED 菜单的 Eject/Reload，不要依赖 Windows 的弹出。

## 8.4 ImgBurn 刻录成功但 Verify 报 Layouts do not match

- `Session Count Not Equal (1/0)` 是布局元数据比较失败，不是数据校验失败；通常 `Miscompares` 仍为 0。
- v1.0.13 起 Disc Information / Track Information / TOC 的 session 状态统一以 `data_lba` 和 `complete` 为准，finalize 后固定报告 1 个 session。
- 已刻好的盘不需要重刻：升级 v1.0.13 后直接用 ImgBurn 的 Verify 验证现有盘。
- 串口会打印 `DISCINFO ... sessions=...` 和 `TOC ... blank=... complete=...`，若仍失败请提供这两行。
- v1.0.14 在 ImgBurn 的虚拟托盘 cycle 时发送 Unit Attention，但 ImgBurn 仍可能保留内部布局缓存。
- v1.0.15 改为真实虚拟托盘：eject 时卸载镜像（主机看到介质消失），load 时从独立保存的路径重新打开镜像。ImgBurn 的 `Cycling Tray before Verify` 因而会完整重读 TOC。升级后可直接 Verify 已有光盘，不需要重刻。
- v1.0.16 补全 `READ TOC format 2` 的 track 1 描述符。Full TOC 必须包含 A0/A1/A2 **和每个 track**；缺 track 时 ImgBurn 的布局比较会得到 0 个 session。

## 8.5 读取速度慢

- 日志中如果每次多块 `SD read ... n=4/8 dt_us≈103000`，是 CMD12 在卡忙时发出，`wait_r1()` 撞 100 ms 超时；v1.0.21 已改为先等卡不忙再发 CMD12。
- 默认固件不再逐条打印 READ；需要看 SD 耗时时刷 `dist/cdrw_emulator_readtrace.uf2`。
- 正常 SD 多块读 4 KiB 应约 3–5 ms，32 KiB 约 25–35 ms；若远高于此，检查卡和模块。
- v1.0.22 的 25 MHz 读取在部分 SDXC 卡/模块上会导致 `f_mount failed FRESULT=1`；v1.0.23 已恢复默认 12 MHz 读取。
- v1.0.26 恢复 CMD25，但按 SD 规范修正：开头只发一次 `0xFC`，后续块直接发数据+CRC，不再每块发 `0xFE`，最后发 `0xFD`。
- v1.0.27 在每块响应后等待 200us 再轮询 busy。
- v1.0.28 修正 CMD25 每个 block 都发送 `0xFC` 起始 token；此前仅首块发送，block 1 会报 write error（F9）。
- v1.0.29 在 CMD25 前发送 ACMD23 预擦除，并缩短块间 settle；若 block 1 再次出现 F9，可把 100us 调回 200us。

## 8.6 读速度与双缓冲

- v1.0.25 起读路径使用双缓冲：USB 发送当前 32 KiB 时，SD 预读下一块，减少串行等待。
- 如果读取仍只有约 420 KB/s，说明瓶颈在 USB BOT 或主机的全速调度，不再是 SD 读取。
- 读缓冲增加 32 KiB RAM；RP2350 内存足够。

## 8.7 20 MHz 写测试（已撤回）

- 20 MHz 写在 block 0 即失败（`resp=00`），失败后卡停在多块写模式，CMD0 返回 `R1=00`，软件回退无法可靠恢复。
- v1.0.31 起不再提供 20 MHz 写版本，默认 12 MHz 写保持稳定。

## 8.8 Windows 资源管理器无法访问空白盘

- 症状：双击空白 CD-RW 报“Windows 无法访问此光盘”，没有“如何使用这张光盘”对话框。
- 原因：空白盘的 `READ TOC` 仍报告 track 1 / session 1，但 Disc Information 是 Empty，Windows 认为有一个损坏的零长度卷。
- v1.0.33 winburn 版对空白盘返回空 TOC（无 track/session）。
- 如果仍然报错，抓串口看 Windows 是否发送 `FORMAT UNIT (04)` 或 `SEND CUE SHEET (5D)`。

## 8.9 Windows 不把设备识别为刻录机

- 检查 `GET CONFIGURATION` 的 Feature Descriptor 是否从 offset 8 开始，`feature_begin()` 不能额外加 8。
- v1.0.34 修复了该偏移；此前 Windows 看到的特性区前 8 字节是空洞，Formattable/Restricted/CD Mastering 全部无效。
- 修复后资源管理器应能弹出“如何使用这张光盘”。
- Feature Descriptor 必须按 Feature Number 升序返回；v1.0.35 已排序。
- 如果仍然没有刻录对话框，检查 Windows 服务 `ImapiService` 是否启动，或在另一台机器上测试。
- 空白盘 TOC 必须给 Windows 一个 track 1 和 leadout = 最大容量，否则 mastered 会没有容量显示且只列 1x。
- v1.0.36 的 `GET PERFORMANCE` Type 3 报告 4x（704 KB/s）写入速度。
- v1.0.37 对空白盘恢复空 TOC：容量 leadout 与 Disc Information 的 Empty 状态不一致会让 Windows mastered 拒绝写入。
- v1.0.38 对空白盘把 `READ FORMAT CAPACITIES` 改为 unformatted（type 1），与 Empty 状态一致；Windows 应发送 FORMAT UNIT 后再写入。
- v1.0.39 返回 unformatted + formatted 两个描述符，并记录 `READ FAIL lba=... blocks=...`。
- v1.0.40 处理负 LBA（0xFFFFFExx）的 CD lead-in 读取，返回零扇区。
- v1.0.41 同时处理 0x37FFFExx 一类高位 lead-in/lead-out 虚拟 LBA。
- v1.0.42 实现 `READ DISC STRUCTURE (0xAD)`。
- v1.0.43 格式化后空白 track 的 leadout 使用容量值。
- v1.0.44 FORMAT UNIT 后进入 Complete，并记录 format descriptor。
- v1.0.45 格式化后空白盘的 Track Information 报告完整 free blocks。
- v1.0.46 格式化后空 track 的 blank 标志为 1。
- v1.0.47 输出 `TRACKINFO` 36 字节；Windows 10 上字段正常。
- v1.0.48 在 FORMAT UNIT 后回到 Empty/Blank。
- v1.0.49 实现 `READ BUFFER (0x3C)`。
- v1.0.50/51 实现 Ricoh Mode Page 0x30（Burn-Free）；v1.0.51 修正了 page 0x30 被放在 INQUIRY 分支的问题。

## 9. 刻录成功但电脑仍显示空盘/旧内容

- 真实光驱需要**关闭会话和 TOC**。确保刻录软件执行了 finalize/close，而不是只写入数据。
- 设备在执行 `CLOSE TRACK/SESSION` 和 `SYNCHRONIZE CACHE` 时会更新 sidecar。
- 尝试 `eject` 再重新插入，或重新插拔 USB。
- 用以下命令检查设备状态：

```bash
python3 tools/cdrw.py info /path/to/DISC01.CDR
```

如果 `meta state` 为 complete 且 `lead-out` 正确，说明固件视角已完成；主机侧可能需要重新扫描：

```bash
sudo blockdev --rereadpt /dev/sr0   # 有些系统不支持，可忽略
sudo eject /dev/sr0
sudo eject -t /dev/sr0
```

## 10. 我想删除光盘里的单个文件

数据 CD-RW 不是随机可写文件系统。请在主机上：

1. 擦除光盘；
2. 生成不含该文件的新 ISO；
3. 重新刻录。

设备 UI 的 `Erase` 或刻录软件的擦除功能都可以完成第 1 步。这是 CD-RW 的标准工作方式。

## 11. `.meta` 损坏

sidecar 损坏时，固件会尝试根据盘面内容推断状态（PVD/UDF/首扇区）。如果推断不对：

```bash
# 查看
python3 tools/cdrw.py info /path/disc.cdr

# 重新标记为空白（不擦除数据，但设备读取会返回 0）
python3 tools/cdrw.py blank /path/disc.cdr
```

不要在不了解状态时删除 `.meta`，因为盘面中可能残留旧数据。

## 12. 主机文件权限

Linux 刻录通常需要 root 或相应组权限：

```bash
sudo wodim -v dev=/dev/sr0 -speed=4 -data image.iso
```

也可以把用户加入 `cdrom` 组（具体组名依发行版）：

```bash
sudo usermod -aG cdrom "$USER"
```

重新登录后生效。

## 13. USB 在“新建空白盘”时断开

`New blank` 需要 `f_expand` 分配 700 MiB，会阻塞数秒，期间 USB 主循环不运行，主机可能超时重置。建议：

- 新建/擦除光盘时先不要在主机上进行刻录或挂载操作；
- 若主机重置了 USB，等待设备重新枚举即可，镜像通常已经创建完成；
- 也可以先在电脑上运行 `tools/cdrw.py create`，再在设备菜单选盘。

## 14. 仍然无法枚举

提供以下信息有助于定位：

```bash
lsusb -v -d 1209:cd01
dmesg | tail -100
sudo sg_inq /dev/sr0
udevadm info -q all -n /dev/sr0
```

以及 OLED 当前显示内容、SD 卡格式、使用的刻录软件和完整日志。
