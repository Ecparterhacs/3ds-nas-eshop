# 群晖 NAS 部署教程

下面以这些示例值说明：

```text
群晖地址：192.168.1.123
DSM 用户：your-user
程序目录：/volume1/homes/your-user/3ds-nas-eshop
游戏目录：/volume2/myfile/3dsrom
服务端口：40441
```

请把示例 IP、用户名和卷号替换成自己的值。程序目录与游戏目录可以位于
不同存储池。

## 1. 准备群晖

在 DSM 的“套件中心”安装 Python 3，并在“控制面板 → 终端机和 SNMP”
临时启用 SSH。确认 Mac 与群晖在同一个局域网。

从 Mac 登录：

```sh
ssh your-user@192.168.1.123
```

在群晖创建目录：

```sh
mkdir -p /volume1/homes/your-user/3ds-nas-eshop/server
mkdir -p /volume1/homes/your-user/3ds-nas-eshop/data/db
mkdir -p /volume1/homes/your-user/3ds-nas-eshop/data/covers
mkdir -p /volume2/myfile/3dsrom/static
exit
```

如果 DSM 用户没有目标共享文件夹权限，请在 DSM 的共享文件夹权限页面
授权该用户读写，不要直接把服务长期以 root 身份运行。

## 2. 从 Mac 复制服务端

某些群晖环境的 SCP 子系统受限。下面使用标准输入管道复制，不依赖 SCP：

```sh
cd /path/to/3ds-nas-eshop

cat server/nas_server.py | \
  ssh your-user@192.168.1.123 \
  "cat > /volume1/homes/your-user/3ds-nas-eshop/server/nas_server.py"

cat server/requirements.txt | \
  ssh your-user@192.168.1.123 \
  "cat > /volume1/homes/your-user/3ds-nas-eshop/server/requirements.txt"
```

## 3. 安装 Python 依赖

登录群晖：

```sh
ssh your-user@192.168.1.123
cd /volume1/homes/your-user/3ds-nas-eshop
python3 -m pip install --user -r server/requirements.txt
```

如果 Pillow 在较老的群晖平台无法安装，列表和下载仍可使用，但
`/api/cover_bmp/<id>` 无法转换封面。优先使用 DSM 套件支持的 Python
版本和对应 Pillow wheel。

## 4. 放置游戏文件

推荐在 Mac Finder 中选择“前往 → 连接服务器”，输入：

```text
smb://192.168.1.123
```

打开对应共享文件夹，将你自行备份的 CIA 放入：

```text
/volume2/myfile/3dsrom/
```

服务端会递归扫描子目录。两种常用布局都支持：

```text
/volume2/myfile/3dsrom/马里奥赛车7.cia

/volume2/myfile/3dsrom/马里奥赛车7/
└── 马里奥赛车7 [0004000000030600].cia
```

如果不用 SMB，单个文件也可通过管道复制：

```sh
cat "/Mac/上的路径/My Game.cia" | \
  ssh your-user@192.168.1.123 \
  "cat > '/volume2/myfile/3dsrom/My Game.cia'"
```

不要把 CIA、数据库、个人游戏清单或封面缓存提交到 Git 仓库。

## 5. 启动服务

在群晖 SSH 会话中运行：

```sh
export ESHOP_GAMES_DIR="/volume2/myfile/3dsrom"
export ESHOP_DATA_DIR="/volume1/homes/your-user/3ds-nas-eshop/data"
export ESHOP_PORT="40441"

cd /volume1/homes/your-user/3ds-nas-eshop
python3 server/nas_server.py
```

看到 `Running on 0.0.0.0:40441` 后，在 Mac 浏览器打开：

```text
http://192.168.1.123:40441/
```

点击“扫描新游戏”，或执行：

```sh
curl http://192.168.1.123:40441/api/scan
curl http://192.168.1.123:40441/api/games
```

第二条命令应返回 `count` 和 `games` 数组。

## 6. 设置开机启动

在 DSM 打开“控制面板 → 任务计划 → 新增 → 触发的任务 → 用户定义的
脚本”：

- 用户：选择对两个目录都有读写权限的普通 DSM 用户。
- 事件：开机。
- 任务设置中的用户定义脚本：

```sh
export ESHOP_GAMES_DIR="/volume2/myfile/3dsrom"
export ESHOP_DATA_DIR="/volume1/homes/your-user/3ds-nas-eshop/data"
export ESHOP_PORT="40441"
cd /volume1/homes/your-user/3ds-nas-eshop
/usr/local/bin/python3 server/nas_server.py \
  >> data/server.log 2>&1 &
```

不同 DSM Python 套件的解释器路径可能不同。先在 SSH 中执行
`command -v python3`，把脚本中的路径换成实际结果。

## 7. 部署客户端供下载

在 Mac 编译完成后：

```sh
cat 3ds-eshop-client.3dsx | \
  ssh your-user@192.168.1.123 \
  "cat > /volume2/myfile/3dsrom/static/3ds-eshop-client.3dsx"

cat 3ds-eshop-client.smdh | \
  ssh your-user@192.168.1.123 \
  "cat > /volume2/myfile/3dsrom/static/3ds-eshop-client.smdh"
```

下载地址：

```text
http://192.168.1.123:40441/static/3ds-eshop-client.3dsx
```

## 8. 更新服务端

先停止旧进程，再重新复制 `nas_server.py`。如果确认源码已更新但行为仍像
旧版，可清理当前程序目录里的缓存后重启：

```sh
find /volume1/homes/your-user/3ds-nas-eshop \
  -type d -name __pycache__ -prune -exec rm -rf {} +
```

这条命令只针对本项目目录。不要对 `/volume1` 或 home 根目录执行递归删除。

## 网络安全

本服务没有账号系统。只允许局域网设备访问，不要在路由器上做公网端口
映射。建议在 DSM 防火墙中将 TCP `40441` 限制到自己的局域网网段。

