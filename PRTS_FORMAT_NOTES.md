# prts 源数据更新问题调查笔记(2026-10-07 凌晨)

## 结论

17 个受影响角色(Ceobe 等)的 prts 新文件**本身数据缺失**(皮肤附件减少),不是我们解析器的问题。已用三个解析器交叉验证:

| 解析器 | Ceobe 新 skel 结果 |
|---|---|
| spine-canvas-3.8.99(我们的) | 88 槽 / 37 皮肤附件 / 动画附件引用 0 |
| spine-ts 3.8 官方 | 相同 |
| **prts 网页 viewer 自己的 runtime**(从 source map 提取的 spine-webgl.js) | **相同** |

prts viewer 也用同一份 torappu meta.json 和同一 URL 加载文件 —— 它渲染的就是这 37 附件版本。用户在荒芜拉普兰德页看到的"完整"是因为**该角色新文件完好**(84 附件),而 17 个受影响角色的新导出有缺陷。

## 关键资产(服务器 /tmp/sv/)

- `spine-webgl.js`(489KB)—— prts 的 spine runtime 未压缩源码(从 SpineViewer.CGdTXEiJ.js.map 提取,UMD 可直接 node 使用)
- `SpineViewer.CGdTXEiJ.js` + `.map` —— 打包产物与 source map
- `SpineApi.ts` / `index.vue` / `SpineViewer.vue` —— viewer 加载逻辑(确认与 torappu meta 同源)
- 已脱敏 node 版:`spine-webgl-node.js`(去掉 export default)+ `prts_parse.cjs`(解析测试脚本)

## 恢复 17 个角色完整数据的路径(按优先级)

1. **spine38 旧文件镜像**:viewer 旧入口把 `static.prts.wiki/spine/` 替换为 `spine38/` —— 疑似旧版资源镜像。需要探测确切目录结构(试过的几个路径 404,需从旧 SPINEDATA 样本拿 prefix 格式)
2. **prts 资源仓库 git 历史**:prts 资源更新走 GitHub 流程,旧 skel 可能在仓库历史/release
3. **web.archive.org**:torappu CDN 文件无快照(已查)
4. 设备端旧数据:用户设备 SD 卡上已下载角色的 PPD_Q(旧数据!)可回传作为样本

## 适配方案(如果旧文件拿不到)

用 prts 的 spine-webgl.js 替换我们工具链的 spine-canvas-3.8.99.js —— 两个 runtime 读当前文件结果一致,所以**替换解析器不会解决 17 个角色的数据缺失**,但能保证与 prts viewer 行为一致(未来 prts 修好导出后新文件可读)。

## 影响

- 17 个角色:已回滚到备份数据(图层完整/动作旧版),动作补全依赖旧文件恢复或 prts 修复导出
- 284 个未验收:转换时需逐角色检查新文件完整性(对比备份层数法已可用)
