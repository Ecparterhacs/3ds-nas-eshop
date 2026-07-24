# 3DS 客户端编译与安装

## 前提

- 已安装自定义固件、Homebrew Launcher 和 FBI 的 Nintendo 3DS。
- 3DS 与 NAS 连接到同一个局域网。
- NAS 使用固定 IP 或 DHCP 静态租约。
- Mac/Linux 已安装 devkitPro 3DS 工具链、devkitARM、libctru、
  citro2d 和 citro3d。

## 编译

```sh
git clone https://github.com/Ecparterhacs/3ds-nas-eshop.git
cd 3ds-nas-eshop

export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

make clean
make NAS_HOST=192.168.1.123 NAS_PORT=40441 -j4
make test
```

`NAS_HOST` 只写 IP 或主机名，不要带 `http://` 和端口。构建参数会覆盖
`include/config.h` 的默认示例值。

工具链较新时 Makefile 使用：

```make
include $(DEVKITARM)/3ds_rules
```

静态库顺序必须保持：

```make
-lcitro2d -lcitro3d -lctru -lm
```

## 安装 `.3dsx`

标准方式是复制到 3DS SD 卡，而不是把 `.3dsx` 当作 CIA 安装。

```text
SD:/
└── 3ds/
    └── 3ds-nas-eshop/
        ├── 3ds-nas-eshop.3dsx
        └── 3ds-nas-eshop.smdh
```

可以关机后取出 SD 卡复制，也可以使用你信任的 3DS FTP 工具传输。复制后
从 Homebrew Launcher 启动。

更新时先退出客户端，再覆盖旧 `.3dsx`。如果仍看到旧界面，删除 SD 卡上
同名旧文件后重新复制，并确认文件大小或 SHA-256。

## 安装模式

### A：极速直装

客户端一边从 NAS 接收 CIA，一边写入 3DS AM 安装服务。双缓冲让网络读取
与安装写入重叠，通常是最快的方式，也不需要先保存完整 CIA。

如果网络中断，安装会取消；下次需要从头开始。

### Y：稳定安装

完整流程：

```text
NAS → SD 上的 .cia.part → 完整 .cia → AM 安装 → 删除完整缓存
```

缓存目录：

```text
SD:/3ds/nas-eshop/cache/
```

下载中按 `B` 会保留 `.cia.part`。服务器支持 HTTP Range 时，下次可以
继续下载；不支持时客户端会安全地从头重下。只有 `AM_FinishCiaInstall`
成功后才删除完整缓存。

## Title ID 安全限制

客户端只接受 Title ID 平台 `0004` 下的这些 SD 内容分类：

- Application
- Demo
- Update
- DLC

系统/NAND 标题会被拒绝。该检查是额外保护，不代表来源未知的 CIA 就安全。
