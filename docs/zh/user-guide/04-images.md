# 04 - 图片输入

> [English](../../en/user-guide/04-images.md) | **中文**

> 读完这页你能：发一张图片并得到关于它的回答。

## 前提

- 必须是 **Flash** 模型（`deepseek-v4-flash`，或名字里带 `flash` / `vision`）。
  reasoner 类模型不接受图片，proJV 会直接不附加，而不是让你吃一个 400。
- 支持 JPEG、PNG、GIF、WebP。

## 怎么发

在消息里任意位置写 `@` 加路径：

```
这个 UI 哪里不对？@C:\shots\before.png
```

路径有空格要加引号：

```
@"D:\my shots\before and after.png"
```

一条消息可以带多张。

## 目录

`@C:\shots\`（以分隔符结尾）会展开成该目录**顶层**的图片，一条消息最多 8 张，
不递归。

## 限制

| 限制 | 值 |
|------|-----|
| 单张大小 | 约 30 MiB |
| 每条消息张数 | 8 |

图片以 base64 内容块发送，大图很费 token。`diagram_tool` 之类生成的 PNG 也可以直接 `@` 进来。

---

下一篇 -> [05 日常使用](05-daily.md) | 想改它 -> [图片输入模块](../dev-guide/module-map.md)
