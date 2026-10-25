# 参与 proJV 开发

**简体中文** | [English](CONTRIBUTING.md)

感谢关注。这份文件是路标，不是手册 —— 细节都在开发指南里。

## 动手之前

1. 先读 [docs/zh/dev-guide/README.md](docs/zh/dev-guide/README.md)：请求链路、
   编译命令、代码约定（线程模型、取消、错误格式、测试缝）。
2. 翻一下 [module-map.md](docs/zh/dev-guide/module-map.md)，找到你的改动属于哪个模块。
3. 在 [recipes.md](docs/zh/dev-guide/recipes.md) 里找对应配方 —— 它按顺序列出要改的文件。

## 编译与测试

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Windows：在 x64 Native Tools 命令行里跑（VS 2019+）。
Linux：先 `sudo apt-get install -y libcurl4-openssl-dev`。

只有 `proJV_tests` 全绿才可合并。新行为要配新测试；修 bug 要先有能复现的测试。

## 分支与提交

- 从 `main` 开分支：`feat/<简短名>`、`fix/<简短名>`、`docs/<简短名>`。
- 提交标题用 `type(scope): summary` —— `feat(tools)`、`fix(skill)`、`docs:`、
  `build:`、`chore(tests)`。
- 正文写清**为什么**，并列出形态变化的地方（新增配置键、新增工具、新增测试缝）。
  评审看的是正文，不是 diff。

## 合并请求

清单见 [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md)：

- 本地测试通过；
- 新增或改变的行为有测试覆盖；
- **中英文档一起改** —— `docs/en/` 与 `docs/zh/` 是镜像，文件名一一对应；
  `README.md` 与 `README_zh.md` 同理；
- 不带敏感信息：绝不提交 `config.toml`、`projv_files/sessions/`、API Key 或日志。

## 会被拒绝的改动

- 把唯一的进程 runner 改成进程池，或去掉它的锁 —— 串行是刻意的
  （见 `src/platform/iprocess_runner.h`）。
- 引入 MCP 层、插件注册表或包管理器。
- 在主题系统之外写死颜色。
- 让技能删除它自行判定为死代码的东西。死代码只报告，不删除。

## 报告问题

用 issue 模板，写清 proJV 版本（状态栏 / `/help`）、系统与终端、模型名、复现步骤，
以及 `projv_files/logs` 里相关的一段。记得把 Key 涂掉。
