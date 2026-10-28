# 04 - Images

> After this page you can send an image and get an answer about it.

## Requirements

- A **Flash** model (`deepseek-v4-flash` or any name containing `flash` /
  `vision`). `deepseek-v4-pro` does not accept images; proJV refuses to
  attach them instead of failing with a 400.
- JPEG, PNG, GIF or WebP.

## Send one

Type `@` followed by a path, anywhere in the message:

```
what is wrong with this UI? @C:\shots\before.png
```

Quote paths that contain spaces:

```
@"D:\my shots\before and after.png"
```

You can attach several files in one message.

## Directories

`@C:\shots\` (trailing separator) expands to the images at the top level of
that directory, up to 8 per message. It does not recurse.

## Limits

| Limit | Value |
|-------|-------|
| Per image size | about 30 MiB |
| Images per message | 8 |

Images are sent as base64 content blocks; large images cost tokens. Screenshot
tags and other tools produce PNGs you can attach the same way.

## The model can fetch an image itself

On a Flash model the agent also has the `view_image` tool: instead of asking you
to paste a path, it calls `view_image(paths=[...])` itself (for instance after a
screenshot tool wrote a file).

The API only accepts images in `user` messages, so proJV does not return pixels
to the tool. It registers an image round, ends the current turn, and appends a
synthetic user message carrying the attachments; the next turn can see them.

- The message is marked `(auto)` in the chat, and stored with
  `synthetic = 1`, so a replayed history never looks like you typed it.
- The tool must be called alone. Other tools in the same round still run and
  their results are kept, but the round stops right after them.
- `view_image` and `run_skill` cannot be queued in the same round; the second
  one is refused.
- Outside the workspace, non-images, more than 8 files or an oversized file are
  refused before anything is sent.

---

Next -> [05 daily use](05-daily.md) | Want to change it -> [image input](../dev-guide/module-map.md)
