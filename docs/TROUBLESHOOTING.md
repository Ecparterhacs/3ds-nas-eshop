# 故障排查

## 客户端只显示少量游戏

按 `SELECT` 打开诊断层，比较：

- HTTP 实际字节数与声明字节数。
- JSON `parsed / expected` 数量。
- 缓冲容量与结果码。

当前客户端会从 256 KiB 自动扩容到 2 MiB，并循环调用
`httpcDownloadData` 直到响应结束。若 JSON 超过 2 MiB，需要提高
`HTTP_MAX_CAPACITY` 后重新编译。

## 中文显示成 `u5b57...`

确认安装的是仓库当前版本。解析器会将 `\uXXXX` 和 UTF-16 surrogate pair
转换为 UTF-8。旧版本或缓存的 `.3dsx` 可能仍显示转义文本。

## 按 A 后像卡死

第一次按 `A` 只进入确认页，第二次按 `A` 才启动安装。连接、CIA 校验和
AM 初始化期间可能短暂停留；进度页会显示当前阶段。

按 `SELECT` 查看诊断信息。不要在安装写入期间强制关机。

## 下载或安装很慢

先比较进度页的速度：

- 速度持续很低：检查 3DS Wi-Fi 信号、2.4 GHz 干扰和 NAS 磁盘负载。
- 下载较快而“从 SD 卡安装”慢：瓶颈通常是 SD 卡写入和 AM 安装。
- `A` 通常比 `Y` 总耗时短；`Y` 的目标是断点续传和稳定性。

## 封面不显示

检查：

```text
http://<NAS_IP>:40441/api/games
http://<NAS_IP>:40441/api/cover_bmp/<GAME_ID>?w=128
```

第二个地址需要服务端安装 Pillow。Web API 中 `has_cover` 应为 `true`。

## NAS 返回 404

- 确认 `ESHOP_GAMES_DIR` 指向真实目录。
- 调用 `/api/scan`。
- 确认数据库里的文件路径没有在移动文件后失效；必要时调用
  `/api/rescan` 重建元数据。

## Python 更新后代码没有生效

停止旧进程，清理项目目录内的 `__pycache__`，再启动。确认没有另一个旧
进程仍占用 `40441`。

## Y 模式提示空间不足

稳定模式在安装期间同时保存完整 CIA 和安装后的 title，需预留约 CIA
大小两倍再加 32 MiB。成功后下载缓存会自动删除。

