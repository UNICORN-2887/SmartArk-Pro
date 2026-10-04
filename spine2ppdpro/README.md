# spine2ppdpro

本地版 PRTS Spine -> PPD 导入与仿真器。

## 启动

```bash
cd spine2ppdpro
python local_server.py --port 8088
```

浏览器打开：

```text
http://127.0.0.1:8088/
```

## 使用

在“PRTS 导入”输入任意一种：

```text
https://prts.wiki/w/缄默德克萨斯
缄默德克萨斯
char_1028_texas2
```

点“下载并转换”。服务会本地完成：

1. 解析干员 `charid`
2. 下载 `正面 / 背面 / 基建` 的 `.skel/.atlas/.png`
3. 转换为 `scene.json + .raw + anims.json`
4. 在页面里加载 `/characters/<角色名>/`

输出目录：

```text
spine2ppdpro/data/spine_raw/<charid>/
spine2ppdpro/data/ppd_out/<charid>/
```

`data/ppd_out/<charid>/` 就是给仿真器和后续设备侧交接用的 PPD 数据目录；中文名记录在 `spine2ppdpro.json`。

也可以用前台命令导入，适合先批量准备数据：

```bash
python local_server.py --import-char "https://prts.wiki/w/缄默德克萨斯"
```

## 依赖

转换器需要 Node.js 和 `@napi-rs/canvas`。本目录优先读取 `spine2ppdpro/tools/node_modules`；没有安装时，会复用仓库现有的：

```text
spine2ppdsimulator/tools/node_modules
```

所以当前仓库内可直接运行，不需要先部署云端。
