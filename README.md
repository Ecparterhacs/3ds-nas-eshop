# 3DS NAS eShop

> 在 Nintendo 3DS 上浏览自己的 NAS 游戏库、查看封面，并将 CIA
> 直接安装到 SD 卡。

3DS NAS eShop 由两部分组成：

- 一个运行在群晖或普通 Linux NAS 上的 Flask 服务，负责扫描文件、
  管理元数据和封面并提供下载接口。
- 一个基于 libctru/citro2d 的 3DS `.3dsx` 客户端，提供接近 eShop
  的双屏界面、Unicode 游戏名、封面和安装进度。

项目不包含游戏、Title Key、系统文件或任何 Nintendo 版权内容。
请仅处理你合法拥有并自行备份的内容。

## 功能

- 浏览数百个 `.cia`、`.3ds` 和 `.3dsx` 文件。
- 解析 JSON `\uXXXX`、UTF-16 surrogate pair 和 `null`。
- 显示中文标题、封面、文件大小、区域和 Title ID。
- `A` 极速直装：NAS 下载和 AM 安装采用双缓冲流水线。
- `Y` 稳定安装：先下载到 SD 卡，支持断点续传，成功后自动删除缓存。
- 安装前校验 CIA Title ID，只允许 SD 卡用户内容类型，拒绝系统标题。
- 显示进度、实时速度、ETA、Title ID 和剩余空间。
- Web 管理页支持扫描、改名、上传封面和下载。
- 可选的 K73 封面匹配脚本，只补齐缺失封面。

## 目录结构

```text
3ds-nas-eshop/
├── include/                 3DS 客户端头文件
├── source/                  3DS 客户端源码
├── tests/                   JSON 与 CIA 安全校验测试
├── server/
│   ├── nas_server.py        NAS Flask 服务
│   ├── requirements.txt     Python 依赖
│   └── env.example          环境变量示例
├── scripts/                 可选封面匹配脚本
├── docs/
│   ├── SYNOLOGY.md          群晖逐步部署教程
│   ├── CLIENT.md            编译、复制和使用客户端
│   └── TROUBLESHOOTING.md   常见问题
└── Makefile
```

## 快速开始

### 1. 部署 NAS 服务

群晖推荐布局：

```text
/volume1/homes/<DSM_USER>/3ds-nas-eshop/
├── server/nas_server.py
├── server/requirements.txt
└── data/
    ├── db/games.db
    └── covers/

/volume2/myfile/3dsrom/
├── 游戏 A.cia
├── 游戏 B/
│   └── 游戏 B [0004000012345600].cia
└── static/
    └── 3ds-eshop-client.3dsx
```

服务端通过环境变量读取这些目录，不要求使用相同卷号：

```sh
export ESHOP_GAMES_DIR="/volume2/myfile/3dsrom"
export ESHOP_DATA_DIR="/volume1/homes/<DSM_USER>/3ds-nas-eshop/data"
export ESHOP_PORT="40441"
python3 server/nas_server.py
```

浏览器打开：

```text
http://<NAS_IP>:40441/
```

然后点击“扫描新游戏”，或者访问：

```text
http://<NAS_IP>:40441/api/scan
```

完整的建目录、从 Mac 复制文件、安装 Python 依赖、后台启动和 DSM
任务计划设置见 [群晖部署教程](docs/SYNOLOGY.md)。

### 2. 编译 3DS 客户端

需要 devkitARM、libctru、citro2d 和 citro3d：

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

make NAS_HOST=192.168.1.123 NAS_PORT=40441 -j4
```

将 `192.168.1.123` 换成 NAS 的固定局域网 IP。输出文件：

```text
3ds-eshop-client.3dsx
3ds-eshop-client.smdh
```

完整教程见 [客户端编译与安装](docs/CLIENT.md)。

### 3. 复制到 3DS

推荐关机取出 SD 卡，创建目录：

```text
SD:/3ds/3ds-nas-eshop/
```

复制并改名为：

```text
SD:/3ds/3ds-nas-eshop/3ds-nas-eshop.3dsx
SD:/3ds/3ds-nas-eshop/3ds-nas-eshop.smdh
```

把 SD 卡放回 3DS，从 Homebrew Launcher 启动。

## 操作

| 按键 | 功能 |
| --- | --- |
| 十字键 | 选择游戏 |
| `L` / `R` | 翻页 |
| `A` | 极速直装；再次按 `A` 确认 |
| `Y` | 下载到 SD 后安装；再次按 `A` 确认 |
| `B` | 取消安装或退出 |
| `X` | 重新读取 NAS 游戏列表 |
| `SELECT` | 开关真机诊断信息 |
| `START` | 退出客户端 |

稳定模式缓存位置：

```text
SD:/3ds/nas-eshop/cache/game_<数据库ID>.cia.part
SD:/3ds/nas-eshop/cache/game_<数据库ID>.cia
```

- 下载中断：保留 `.cia.part`，下次选择同一游戏会尝试续传。
- 下载完成但安装失败：保留 `.cia`，下次直接重试安装。
- AM 确认安装成功：自动删除 `.cia`。
- 稳定模式安装时大约需要 CIA 文件大小两倍的可用空间。

## 封面

Web 管理页可以给单个游戏上传封面或填写图片 URL。还可以运行：

```sh
ESHOP_NAS=http://127.0.0.1:40441 \
  python3 scripts/k73_auto_covers.py --dry-run

ESHOP_NAS=http://127.0.0.1:40441 \
  python3 scripts/k73_auto_covers.py
```

脚本只更新没有封面的游戏。第三方页面结构随时可能变化；请遵守来源网站
的使用条款和访问频率限制。抓取生成的目录缓存与匹配报告不会提交到仓库。

## 测试

```sh
make test
```

测试使用合成 CIA 头部，不需要也不会读取真实游戏。

## 安全说明

- Flask API 当前没有登录验证，只应在可信局域网内运行。
- 不要把 `40441` 端口映射到互联网。
- 建议给 NAS 设置固定 DHCP 租约，并在防火墙中只允许局域网访问。
- `/api/rescan` 会重建元数据库，但不会删除游戏文件。
- 客户端的直接安装仍属于高权限操作；安装期间不要关机或拔出 SD 卡。

## 开源许可

代码使用 [MIT License](LICENSE)。Nintendo、Nintendo 3DS、FBI 及其他名称
归各自权利人所有；本项目与 Nintendo 无关。
