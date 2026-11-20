# 原始串口直通固件

`m3b_at_terminal/` 完整保留最初用户提供的 7 个文件，与 Git 提交
`92eff93e5d4fb3d868ed974ed2744a2cfab6d3e5` 逐字节一致。
其中原始 README 的菜单 14 编译说明属于原项目；在本项目使用下列命令。

在上一级 `m3b_at_terminal` 项目目录运行：

```sh
python3 build_terminal.py
```

生成 `build/m3b_at_terminal.bin`，不会覆盖自动测试固件
`build/m3b_v3_autotest.bin`。构建记录为 `build/terminal_manifest.json`。
自动测试版源码保留在上一级目录，仍用 `python3 build.py` 编译。

串口直通版：USB CDC 115200，RF UART 921600 8N1。输入 `ATI`、
`AT-DEF` 等命令并回车，回复原样转发。不自动测试，没有启动提示；
`RUN`、`PD`、`SBTL` 不再是本地测试指令，控制 RF 时应输入完整 AT 命令。
烧录方式同自动测试版，STM32 内部 Flash 起始地址为 `0x08000000`。
