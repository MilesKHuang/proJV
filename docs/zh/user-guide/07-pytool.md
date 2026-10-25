# 07 - pytool

> [English](../../en/user-guide/07-pytool.md) | **中文**

> 读完这页你能：让 AI 调用你自己写的 Python 工具。

## 什么时候用

用 Python 更省事的事：数据处理、画图、调某个库。proJV 启动时扫描
`projv_files/pytool/` 下的每个目录。

## 两个文件

```
projv_files/pytool/get_time/
├── tool.json     名字、描述、参数
└── main.py       从 stdin 读 JSON 参数，往 stdout 打结果
```

`tool.json`：

```json
{
  "name": "get_time",
  "description": "Get the current system time.",
  "parameters": []
}
```

`main.py`：

```python
import json, sys, time
args = json.loads(sys.stdin.read() or "{}")
print(time.strftime("%Y-%m-%d %H:%M:%S"))
```

契约就这些：stdin 进 JSON，stdout 出文本，退出码 0。

## 带参数

```json
{
  "name": "word_count",
  "description": "Count words in a file.",
  "parameters": [
    {"name": "path", "type": "string", "description": "file to read", "required": true}
  ]
}
```

```python
import json, sys
args = json.loads(sys.stdin.read() or "{}")
print(len(open(args["path"], encoding="utf-8").read().split()))
```

## 依赖

在 `main.py` 旁边放 `requirements.txt`。解释器用 `config.toml` 的 `python_path`
（留空 = PATH 里的 `python`）。

## 几条必须知道的规矩

- **同一时间只有一个子进程。** pytool 和 `exec_shell` 共用一个进程 runner，一个
  长 pytool 会挡住别的 shell 调用。这是有意的：只有一个子进程能被 `Esc` 可靠中断。
- **不要交互。** 没有可点的东西、不要等待输入；读 stdin、写 stdout。
- **错误要可读。** 打清楚原因并以非零码退出，agent 能看到文本并据此调整。

## 怎么确认成功

问它：`用 word_count 工具统计 README.md`。第一次会出现在审批弹窗里。如果 agent
看不到这个工具，先检查 `tool.json` 能不能解析（解析失败的会在启动时被静默跳过）。

---

下一篇 -> [08 参考](08-reference.md) | 想改它 -> [pytool 管理](../dev-guide/module-map.md)
