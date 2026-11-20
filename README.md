# 菜单 14：基本 AT 终端

通过电脑 USB 终端输入 AT 命令，STM32 转发给 RF 模块，并原样显示模块回复。
固定使用内部 HSI16 → PLL → 32 MHz，RF 为 PC10 TX / PC11 RX、921600 8N1，
电脑 USB CDC 终端设为 115200。

## 使用

输入命令后回车，例如：

    ATI
    AT-MEUI?
    AT-MAI?

CR、LF、CRLF 均可结束一行，固件在命令末尾添加一个 CR。
支持 \t、\x1A、\r、\n、\\ 转义，便于输入包含控制字节的 AT 命令。
退格可修改当前输入，Ctrl-C 清除当前输入；每条命令等待完整回复后再发送下一条。
输入上限为 1023 个字符，过长的行或非法转义整行丢弃。
固件只显示 RF 原始回复，不输出启动提示、收发日志或诊断信息。

## 编译和烧录

在本仓库目录运行 `python3 build.py`，生成 `build/m3b_at_terminal.bin`。
构建记录为 `build/terminal_manifest.json`，不依赖 C 客户端库。
脚本复用服务器同级 `mioty-m3board/.tools/linux` 的 Arduino CLI 和 STM32 core 3.0.0，
也可通过 `python3 build.py --tools /path/to/tools` 指定已安装的工具目录。

复制到电脑后，用 STM32CubeProgrammer 通过 USB DFU 烧录，起始地址 0x08000000。
完成后关闭 BOOT，按 RESET，再打开 USB 终端。

## 文件

- m3b_at_terminal.ino：USB 输入、按行发送和原始回复转发。
- TerminalCore.h：转义解码及 CR 结尾。
- RfDmaRx.cpp / RfDmaRx.h：DMA 循环接收，由 setup 初始化、loop 轮询。
- ClockConfig.cpp：内部 HSI16 和独立 USB HSI48 时钟。
- build_opt.h：Arduino 串口接收缓冲区大小。

DMA 使用 DMA1 通道 3、请求 5、2048 字节循环区及其共享中断，通道 2 未使用。
USB 输出暂满时缓存在内存中，长时间不读取仍可能丢数据。
编译及 host 测试不替代当前固件的实板验证。

## 分支

- `master`：串口直通版，手动输入完整 AT 命令，不自动测试。
- `autotest`：V3 客户端自动测试版，使用该分支的 `python3 build.py` 构建，详见该分支 README。
