# 换 CPU 维修前的环境备份方案(2026-10-06)

## 一、会发生什么 / 不会发生什么

| 风险 | 说明 | 概率 |
|---|---|---|
| 硬盘数据丢失 | 换 CPU/修主板是**硬件层操作**,理论上不动硬盘。但维修店可能:①重装系统 ②更换硬盘 ③磁盘克隆时覆盖 | 中 |
| 环境重置 | 只要 C 盘数据还在,conda/IDF/node/Claude 都在,**不会被重置**(它们只是文件+环境变量)。唯一会丢的是维修店重装系统 | 低-中 |
| Claude 对话历史丢失 | 历史存在 `C:\Users\HP\.claude\`,在 C 盘。同上 | 低-中 |
| 激活/密钥丢失 | Windows 激活绑定主板(数字权利激活),**换主板/CPU 后激活可能失效**,需要重新激活 | 高 |
| 路径"被遗忘" | 不会。路径存在文件中,不在"记忆"里 | 无 |

**核心结论:只要硬盘不被重装/更换,一切都在。最大风险是维修店顺手重装系统。**

## 二、必须做的备份(按优先级)

### 1. Claude 对话历史(项目紧急度最高)
```
C:\Users\HP\.claude\projects\    ← 全部项目的对话历史(JSONL)
```
整个 `.claude` 目录打包即可(几 GB,压缩后几百 MB)。

### 2. 项目代码
```
E:\Passport\espp4\...\xiaozhi-esp32sp1   ← 固件仓库(git 已提交的不会丢,未提交的会丢!)
```
**先 `git status` 看有没有未提交改动,全部 commit 并 push 到远端**(当前有未提交的修改)。

### 3. conda 环境
```powershell
conda env export -n brain > brain_env.yml        # 导出环境定义
conda env list                                   # 记录环境列表
```
brain 环境本体在 `C:\Users\HP\.conda\envs\brain\` 或安装目录,空间足够的话整体打包;不够的话靠 yml 重建(下载要时间)。

### 4. ESP-IDF 与工具链
```
E:\Passport\esp32\                               ← IDF 本体
C:\Users\HP\.espressif\                          ← 工具链(riscv/gcc 等,几个 GB)
```
这两个是**纯文件**,只要 E/C 盘在就不用重装。为保险,记录版本号:
- IDF: v5.5.1(E:\Passport\esp32\v5.5.1\esp-idf)
- 工具链: C:\Users\HP\.espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119

### 5. node
```
node -v; npm -v   # 记录版本
E:\Passport\espp4\...\spine2ppdsimulator\node_modules\   # 项目内 node_modules 不用备份(npm install 可重建)
```

### 6. 环境变量(记录下来,重装系统时重建)
```
IDF_PATH = E:\Passport\esp32\v5.5.1\esp-idf
PATH 增加:C:\Users\HP\.espressif\tools\...
conda 路径:C:\ProgramData\miniconda3 或 C:\Users\HP\miniconda3
```

## 三、与维修店的三条硬性要求(写进维修单)

1. **不得重装/恢复操作系统,不得动硬盘数据**
2. 如必须更换硬盘,**必须归还旧硬盘**
3. 维修完成后开机验证原桌面/文件完好再付款

## 四、回来后 10 分钟恢复清单

1. 开机 → 检查 E 盘项目、C 盘 .claude、.espressif 是否还在
2. Windows 激活失效 → 用原激活方式重新激活
3. `conda activate brain` 验证;失败则 `conda env create -f brain_env.yml`
4. `idf.py build` 验证 IDF 链;失败检查 IDF_PATH
5. 打开 Claude Code,确认历史在(`/resume` 可继续本会话)

## 五、其他隐患

- **磁盘加密(BitLocker)**:如果开了,换主板后 TPM 变化可能要求恢复密钥 —— 提前在微软账户导出恢复密钥
- **维修店要你的开机密码**:建议临时建一个"维修专用账号",不要给主账号密码
- 电源适配器/电池:笔记本送修记得拔电池(如可拆卸)
