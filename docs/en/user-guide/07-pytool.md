# 07 - pytool

> After this page the agent can call a Python tool you wrote.

## When to use it

pytool is for anything that is easier in Python than in C++: data munging,
plots, calling a library. proJV discovers every folder under
`projv_files/pytool/` at startup.

## Two files

```
projv_files/pytool/get_time/
├── tool.json     name, description, parameters
└── main.py       reads JSON args on stdin, prints the result on stdout
```

`tool.json`:

```json
{
  "name": "get_time",
  "description": "Get the current system time.",
  "parameters": []
}
```

`main.py`:

```python
import json, sys, time
args = json.loads(sys.stdin.read() or "{}")
print(time.strftime("%Y-%m-%d %H:%M:%S"))
```

That is the whole contract: JSON in on stdin, text out on stdout, exit code 0.

## With parameters

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

## Dependencies

Add `requirements.txt` next to `main.py`. proJV uses the interpreter configured
in `config.toml` (`python_path`; empty = `python` from `PATH`).

## Rules that matter

- **One process at a time.** pytool and `exec_shell` share a single process
  runner, so a long pytool blocks other shell calls. That is deliberate: only
  one child process can be interrupted by `Esc`.
- **No interactivity.** Nothing to click, no prompts; read stdin, write stdout.
- **Errors are readable.** Print a clear message and exit non-zero; the agent
  sees the text and can react.

## Check it worked

```
/skill list          # unrelated, but proves the app is alive
```
Then ask: `use the word_count tool on README.md`. The tool appears in the
approval dialog the first time. If the agent cannot see it, check that
`tool.json` parses (a broken JSON file is skipped silently at startup).

---

Next -> [08 reference](08-reference.md) | Want to change it -> [pytool manager](../dev-guide/module-map.md)
