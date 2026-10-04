# RunningHub API 使用说明(资源制作同学版)

> 本文档由数智方舟项目负责人授权编写。以下 API Key 为负责人的 RunningHub 账号,
> **仅供资源制作同学本人使用**,调用产生的 RH 币费用由负责人承担。

## 0. 授权与安全约定

- ✅ 允许:调用本文档中的工作流/自己找到的工作流、上传素材、下载结果
- ❌ 禁止:把 API Key 发给第三方、提交到 GitHub/公开仓库、硬编码进开源代码
- ⚠️ 每次任务调用都会消耗负责人账号的 **RH 币**,请确认工作流必要后再跑;
  跑大批量前先单张试跑一次验证结果与成本

## 1. API Key

```
b020001dea3944dfbdbc4ad5b7e66ea8
```

用法:所有请求带请求头

```
Authorization: Bearer b020001dea3944dfbdbc4ad5b7e66ea8
```

## 2. 调用约定(RunningHub OpenAPI v2)

| 项 | 值 |
|---|---|
| Base URL | `https://www.runninghub.ai/openapi/v2` |
| 鉴权 | `Authorization: Bearer <key>` |
| 提交任务 | `POST /run/{endpointType}/{endpointId}`(endpointType: `workflow` 或 `ai-app`) |
| 查询状态 | `POST /query`,body `{"taskId": "..."}`;`status` ∈ RUNNING/SUCCESS/FAILED/CANCEL |
| 上传素材 | `POST /media/upload/binary`,multipart 字段 `file` → 返回 `fileName` 与 `download_url` |

### 提交任务 body(通用格式)

```json
{
  "nodeInfoList": [
    {"nodeId": "1", "fieldName": "image", "fieldValue": "<上传返回的 fileName>"},
    {"nodeId": "9", "fieldName": "seed", "fieldValue": 123456789}
  ],
  "instanceType": "plus"
}
```

- `nodeId`/`fieldName` 来自工作流的 **API JSON**(在 RunningHub 工作流页面"发布 API"
  后导出,每个节点一个 nodeId,入参用 fieldName 映射)
- `instanceType`:**用 `plus`(48G 显存)**;`default`(24G)跑拆图类工作流会显存溢出

### 已知工作流端点(负责人账号内)

| 用途 | endpointType | endpointId |
|---|---|---|
| see-through 拆图(默认) | ai-app | `2041193510195437570` |
| see-through 拆图(备用) | workflow | `2091171861853184001` |
| 嘴部 inpaint | workflow | `2095059613594210306` |

> 你新找到的工作流:在 RunningHub 页面导入后点"发布 API"即可获得自己的
> endpointType/endpointId,按上面格式调用即可,费用同样记在负责人账号。

### 结果下载

任务 SUCCESS 后,`/query` 返回的 `outputs` 里是各输出节点的 URL(或 fileName),
直接 HTTP GET 下载即可(素材上传返回的 `download_url` 同理)。

## 3. 现成脚本(推荐直接用)

服务器上已有封装好的客户端:`/opt/szfz/runninghub_client.py`

```bash
# 单张试跑(推荐先跑这个,验证全链路与单次成本)
python3 /opt/szfz/runninghub_client.py test <输入图片> <输出目录>

# 批量
python3 /opt/szfz/runninghub_client.py batch <图片目录> <输出目录> --pattern "*.png"
```

环境变量(脚本默认值已配好,一般不用改):

```bash
export RH_API_KEY=b020001dea3944dfbdbc4ad5b7e66ea8
export RH_ENDPOINT=2041193510195437570
export RH_ENDPOINT_TYPE=ai-app
export RH_INSTANCE_TYPE=plus
```

脚本已处理:上传重试、任务轮询(网络异常不判死)、失败原因提取、结果落盘
(输出 `<输出目录>/<名字>/seethrough_00001.psd` + 预览图)。

## 4. 费用与配额提示

- RH 币余额可以在 https://www.runninghub.ai 账号中心查看
- 拆图类单张成本取决于工作流节点数/步数;`num_inference_steps` 默认已调 60,
  `tblr_split` 已关(头饰保留的关键参数,不要改回)
- 大量调用前先问一下负责人,避免一次跑空余额
