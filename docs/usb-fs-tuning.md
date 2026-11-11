# 把 RP2350 USB Full-Speed 跑到接近 12 Mbps 的改进方案

## 1. 先明确上限

USB Full-Speed：

```text
12 Mbps / 8 = 1.5 MB/s（理论）
BOT + MSC + 协议开销后：约 1.0–1.2 MB/s（实际）
```

对应 CD 倍速：

```text
1x  = 150 KB/s
4x  = 600 KB/s
8x  = 1.2 MB/s
```

所以即使把 RP2350 优化到极限：

- **读取最多约 1.0–1.2 MB/s，约 7x–8x**
- **写入受 SD 卡和 CMD25 限制，约 0.6–0.8 MB/s**
- **24x/52x 不可能**，必须换 USB HS 平台

当前实测：

```text
读 544 KB/s
写 351 KB/s
```

还有明显空间。

---

## 2. 当前瓶颈判断

### 读路径

现有结构：

- `IO_CHUNK = 32768`
- 两个 32 KB 缓冲：`io`、`io2`
- USB 发送当前块时，SD 预读下一块
- SD SPI 12 MHz，实际约 10.7 MHz
- `sd_read_blocks()` 使用 CMD18 + 逐块 SPI 读写

问题：

1. **SD SPI 12 MHz 本身只有 1.5 MB/s 理论值**，实际 10.7 MHz 约 1.34 MB/s
2. **逐块 512B 读写 + 轮询**，CPU 参与多，块间有空隙
3. 双缓冲只隐藏了一个块的延迟，遇到卡内部延迟/CMD18 启动仍会等
4. USB FS 的 32 KB 传输本身不是瓶颈

### 写路径

现有结构：

- USB 接收 32 KB
- 再同步写 SD
- CMD25 多块写，每块之间：
  - `sleep_us(100)`
  - `wait_not_busy`
- 没有写双缓冲

问题：

1. **USB 接收和 SD 写入是串行的**
2. 每块 100µs 固定等待
3. SD 写时钟 12 MHz，20 MHz 实测失败
4. 卡的编程 busy 时间是主要部分

---

## 3. 软件改进（按收益排序）

### 3.1 SD SPI 改用 DMA

当前逐块 `spi_read_blocking` / `spi_write_blocking`，建议：

- 32 KB 整块 DMA
- CMD18 读：DMA 接收 32 KB
- CMD25 写：DMA 发送 32 KB
- CPU 只处理命令、token、CRC、busy

收益：

- 读：减少块间空隙，预计 +20–40%
- 写：减少 CPU 占用，为双缓冲让路

RP2350 的 SPI 支持 DMA，改动集中在 `sd_spi.c`。

### 3.2 读路径加第三缓冲

现在两块 32 KB：

```text
USB 发块 A，SD 读块 B
```

改成三块：

```text
USB 发块 A，SD 读块 B，SD 预读块 C
```

收益：

- 隐藏 CMD18 启动和卡内部延迟
- 适合 32 KB 大块
- RAM 只多 32 KB，RP2350 足够

### 3.3 写路径双缓冲

现在：

```text
USB 收 32 KB -> SD 写 32 KB -> USB 收下一块
```

改成：

```text
USB 收块 B，同时 SD 写块 A
```

可以直接复用 `io2`，或者新增一个 32 KB 写缓冲。

收益：

- 写速度接近 `max(USB 接收时间, SD 写入时间)`
- 预计从 351 KB/s 提升到 550–700 KB/s

### 3.4 缩短 CMD25 块间等待

当前：

```c
sleep_us(100);
wait_not_busy(500);
```

100µs × 64 块 = 6.4 ms / 32 KB，约占 7%。

建议：

- 先 `wait_not_busy`，只在 MISO 仍高时补一个短延迟
- 或从 100µs 降到 20–50µs
- 必须保持 CMD25 稳定，不能为了速度重新引入 `F9`

### 3.5 读时钟尝试 16–20 MHz

当前：

```text
CDRW_SD_READ_HZ = 12 MHz
实际 10.7 MHz
```

之前 25 MHz 在 128GB SDXC 上 `f_mount` 失败，但 16/18/20 MHz 没系统试过。

建议：

- 读：16 → 18 → 20 MHz 逐级测试
- 写：保持 12 MHz（20 MHz 写已在 block 0 失败）
- 每次测试：
  - `f_mount`
  - 连续读 100 MB
  - 校验 SHA256
  - 观察是否掉卡

如果 20 MHz 稳定，SPI 理论到 2.5 MB/s，足够喂满 USB FS。

### 3.6 减少 FatFs 开销

当前通过 `f_read` 读镜像。可以：

- 镜像文件连续时，缓存 **起始簇/扇区映射**
- 大块读直接走 `disk_read`
- 小文件/碎片文件仍走 FatFs

收益取决于文件是否连续。新建 `.CDR` 通常是连续的。

### 3.7 USB 侧

- 保持 32 KB 块，TinyUSB 的 `uint16_t` 长度限制不适合强行 64 KB
- 确保 USB 中断优先级高于 OLED/编码器
- 读传输期间不要刷新 OLED
- 不要打开逐 READ 的 UART 日志

---

## 4. 硬件改进

### 4.1 SD 模块和走线

当前蓝色 SD 模块通常带电平转换/稳压，12 MHz 可以，20 MHz 以上容易出问题。

建议：

- 用 **3.3V 原生 microSD 转接板**，不要经过电平转换
- SPI 线尽量短，最好 <10 cm
- 每根信号串 **22–33Ω**
- CMD/DAT 加 **10k 上拉**
- 卡座电源脚加 **100µF + 100nF**
- GND 尽量粗、尽量短

### 4.2 电源

SD 卡写入瞬间电流可到 100–200 mA。检查：

- 3.3V LDO 余量
- USB VBUS 到 3.3V 的压降
- 卡座附近去耦

### 4.3 卡本身

- 不同卡在 SPI 模式下差异很大
- 128GB SDXC 在 25 MHz 失败，不代表小容量卡也失败
- 可以准备一张 2GB/8GB 卡做高速测试

### 4.4 不建议现在做 PIO SD 4-bit

PIO 4-bit SD 可以到 12–25 MB/s，但：

- USB FS 上限只有 1.2 MB/s
- 4-bit SD 对 12 Mbps 目标是过度设计
- 工作量大，收益被 USB 吃掉

只有换 USB HS 平台时才值得做 4-bit SD。

---

## 5. 推荐实施顺序

### 第一阶段：纯软件，不动硬件

1. SD SPI DMA
2. 写双缓冲
3. 读三缓冲
4. CMD25 等待优化
5. 回归 ImgBurn/Kylin

预期：

```text
读 650–800 KB/s
写 500–650 KB/s
```

### 第二阶段：时钟和硬件

1. 换 3.3V 原生 SD 转接板
2. 缩短走线、加串阻和去耦
3. 读时钟 16/18/20 MHz 逐级测试
4. 写保持 12 MHz

预期：

```text
读 850–1100 KB/s
写 600–750 KB/s
```

### 第三阶段：逼近 USB FS 上限

1. 读三缓冲 + DMA + 20 MHz
2. 写双缓冲 + DMA + 卡写入优化
3. 确保 USB 中断优先

预期：

```text
读 1.0–1.1 MB/s
写 0.65–0.8 MB/s
```

这已经接近 USB FS + BOT 的实际极限。

---

## 6. 不建议做的事

- **不要为了 12 Mbps 上 PIO SD 4-bit**：USB 先瓶颈
- **不要继续试 20 MHz 写**：已在 block 0 失败
- **不要用 PIO USB Host 替代 SD**：双 FS 瓶颈更慢
- **不要指望 24x**：USB FS 物理上限

---

## 7. 结论

想把现有 RP2350 硬件跑满 12 Mbps：

1. **SD SPI DMA**
2. **写双缓冲**
3. **读三缓冲**
4. **读时钟 16–20 MHz**
5. **短走线、3.3V 原生 SD、去耦**

目标不是 1.5 MB/s，而是 **1.0–1.1 MB/s 读、0.65–0.8 MB/s 写**，约 **7x–8x CD**。

这已经是 USB Full-Speed 的合理极限。真正的 24x/52x 仍然必须换 USB High-Speed 平台。
