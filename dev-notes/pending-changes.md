# 待验证的修改

> 此文件记录已修改代码但尚未经过实际验证的变更。
> 验证通过后，将条目移至 `verified-changes.md`；验证失败则在此记录回滚步骤。

---

## 待验证列表

（当前无待验证项）

---

## 回滚指南

如需回滚某次修改，使用 git:
```bash
# 查看所有改动
git diff

# 回滚单个文件
git checkout -- <file_path>

# 回滚所有未提交的改动
git checkout -- .
```
