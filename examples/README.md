# 示例光盘

`demo.iso` 是一个约 500 KiB 的 ISO9660/Joliet 小光盘，包含：

- `README.TXT`：说明
- `DATA.BIN`：256 KiB 可校验数据

把 `demo.iso` 复制到 microSD 根目录或 `/discs`，在设备 OLED 菜单选择它，电脑就会出现一张只读 CD-ROM。如果只是验证“电脑是否能认成光驱”，用它最快。

生成方式：

```bash
python3 tools/cdrw.py mkiso examples/demo_folder examples/demo.iso --volume-id RPCDRW_DEMO
```

要测试可写 CD-RW，请按 README 用 `tools/cdrw.py create` 创建一张 700 MiB 的 `.CDR`，再用 ImgBurn / wodim 把 `demo.iso` 刻进去。
