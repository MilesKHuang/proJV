# 04 - Images

> After this page you can send an image and get an answer about it.

## Requirements

- A **Flash** model (`deepseek-v4-flash` or any name containing `flash` /
  `vision`). Reasoner-style models do not accept images; proJV refuses to
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

---

Next -> [05 daily use](05-daily.md) | Want to change it -> [image input](../dev-guide/module-map.md)
