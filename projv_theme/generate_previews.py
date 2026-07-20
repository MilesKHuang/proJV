"""Generate theme preview PNGs using pure Python (no Pillow needed)."""
import json, struct, zlib, os, math

def hex_to_rgb(h):
    h = h.lstrip('#')
    return (int(h[0:2],16), int(h[2:4],16), int(h[4:6],16))

def make_png(width, height, pixels):
    def chunk(ctype, data):
        c = ctype + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    raw = b''
    for y in range(height):
        raw += b'\x00'
        for x in range(width):
            r, g, b = pixels[y * width + x]
            raw += bytes([r, g, b])
    idat = chunk(b'IDAT', zlib.compress(raw))
    iend = chunk(b'IEND', b'')
    return sig + ihdr + idat + iend

def rect(pixels, w, x, y, rw, rh, color):
    for dy in range(rh):
        for dx in range(rw):
            px, py = x + dx, y + dy
            if 0 <= px < w and 0 <= py < (len(pixels)//w):
                pixels[py * w + px] = color

def rounded_rect(pixels, w, x, y, rw, rh, color, radius=6):
    for dy in range(rh):
        for dx in range(rw):
            px, py = x + dx, y + dy
            if px < 0 or px >= w or py < 0 or py >= len(pixels)//w:
                continue
            ok = True
            if dx < radius and dy < radius:
                ok = (dx - radius)**2 + (dy - radius)**2 <= radius**2
            elif dx >= rw - radius and dy < radius:
                ok = (dx - (rw - 1 - radius))**2 + (dy - radius)**2 <= radius**2
            elif dx < radius and dy >= rh - radius:
                ok = (dx - radius)**2 + (dy - (rh - 1 - radius))**2 <= radius**2
            elif dx >= rw - radius and dy >= rh - radius:
                ok = (dx - (rw - 1 - radius))**2 + (dy - (rh - 1 - radius))**2 <= radius**2
            if ok:
                pixels[py * w + px] = color

FONT = {
'A':[0,1,1,1,0,1,0,0,0,1,1,1,1,1,1,1,0,0,0,1,1,0,0,0,1],
'B':[1,1,1,1,0,1,0,0,0,1,1,1,1,1,0,1,0,0,0,1,1,1,1,1,0],
'C':[0,1,1,1,1,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,1,1,1],
'D':[1,1,1,1,0,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1,1,1,1,1,0],
'E':[1,1,1,1,1,1,0,0,0,0,1,1,1,1,0,1,0,0,0,0,1,1,1,1,1],
'F':[1,1,1,1,1,1,0,0,0,0,1,1,1,1,0,1,0,0,0,0,1,0,0,0,0],
'G':[0,1,1,1,1,1,0,0,0,0,1,0,1,1,1,1,0,0,0,1,0,1,1,1,1],
'H':[1,0,0,0,1,1,0,0,0,1,1,1,1,1,1,1,0,0,0,1,1,0,0,0,1],
'I':[1,1,1,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,1,1,0,0],
'L':[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,1,1,1,1],
'M':[1,0,0,0,1,1,1,0,1,1,1,0,1,0,1,1,0,0,0,1,1,0,0,0,1],
'N':[1,0,0,0,1,1,1,0,0,1,1,0,1,0,1,1,0,0,1,1,1,0,0,0,1],
'O':[0,1,1,1,0,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1,0,1,1,1,0],
'P':[1,1,1,1,0,1,0,0,0,1,1,1,1,1,0,1,0,0,0,0,1,0,0,0,0],
'R':[1,1,1,1,0,1,0,0,0,1,1,1,1,1,0,1,0,1,0,0,1,0,0,1,1],
'S':[0,1,1,1,1,1,0,0,0,0,0,1,1,1,0,0,0,0,0,1,1,1,1,1,0],
'T':[1,1,1,1,1,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0],
'U':[1,0,0,0,1,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1,0,1,1,1,0],
'V':[1,0,0,0,1,1,0,0,0,1,0,1,0,1,0,0,1,0,1,0,0,0,1,0,0],
'W':[1,0,0,0,1,1,0,0,0,1,1,0,1,0,1,1,1,0,1,1,1,0,0,0,1],
'Y':[1,0,0,0,1,0,1,0,1,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0],
'a':[0,0,0,0,0,0,1,1,1,0,0,0,0,0,1,0,1,1,1,1,1,0,0,1,1],
'b':[1,0,0,0,0,1,1,1,1,0,1,0,0,0,1,1,0,0,0,1,1,1,1,1,0],
'c':[0,0,0,0,0,0,1,1,1,0,1,0,0,0,0,1,0,0,0,0,0,1,1,1,0],
'd':[0,0,0,1,0,0,1,1,1,1,1,0,0,0,1,1,0,0,0,1,0,1,1,1,1],
'e':[0,0,0,0,0,0,1,1,1,0,1,0,0,0,1,1,1,1,1,0,0,1,1,1,0],
'f':[0,0,1,1,0,0,1,0,0,0,1,1,1,0,0,0,1,0,0,0,0,1,0,0,0],
'g':[0,0,0,0,0,0,1,1,1,1,1,0,0,0,1,0,1,1,1,1,0,0,0,0,1],
'h':[1,0,0,0,0,1,1,1,1,0,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1],
'i':[0,1,0,0,0,0,0,0,1,1,0,0,0,1,0,0,0,1,0,0,1,1,1,0,0],
'k':[1,0,0,0,0,1,0,0,1,0,1,1,1,0,0,1,0,0,1,0,1,0,0,0,1],
'l':[1,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,1,1,1,0,0],
'm':[0,0,0,0,0,1,1,1,1,0,1,0,1,0,1,1,0,1,0,1,1,0,0,0,1],
'n':[0,0,0,0,0,1,1,1,1,0,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1],
'o':[0,0,0,0,0,0,1,1,1,0,1,0,0,0,1,1,0,0,0,1,0,1,1,1,0],
'p':[0,0,0,0,0,1,1,1,1,0,1,0,0,0,1,1,1,1,1,0,1,0,0,0,0],
'r':[0,0,0,0,0,1,0,1,1,0,1,1,0,0,0,1,0,0,0,0,1,0,0,0,0],
's':[0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0,0,1,1,1,1,1,0],
't':[0,1,0,0,0,1,1,1,0,0,0,1,0,0,0,0,1,0,0,0,0,0,1,1,0],
'u':[0,0,0,0,0,1,0,0,0,1,1,0,0,0,1,1,0,0,0,1,0,1,1,1,1],
'v':[0,0,0,0,0,1,0,0,0,1,0,1,0,1,0,0,1,0,1,0,0,0,1,0,0],
'y':[0,0,0,0,0,1,0,0,0,1,0,1,0,1,0,0,0,1,0,0,1,1,0,0,0],
' ':[0]*25, '.':[0,0,0,0,0,0,0,0,0,0,0,0,1,1], ':':[0,0,1,1,0,0,1,1,0,0],
'#':[0,1,0,1,0,1,1,1,1,1,0,1,0,1,0,1,1,1,1,1,0,1,0,1,0],
'[':[1,1,0,1,0,0,1,0,0,1,0,0,1,1,0],']':[1,1,0,0,1,0,0,1,0,0,1,0,1,1,0],
'x':[0,0,0,0,0,1,0,0,0,1,0,1,1,1,0,0,1,1,1,0,1,0,0,0,1],
'-':[0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0],
'+':[0,0,1,0,0,0,0,1,0,0,1,1,1,1,1,0,0,1,0,0,0,0,1,0,0],
'(':[0,1,0,1,0,0,1,0,0,1,0,0,0,1,0],')':[0,1,0,0,0,1,0,0,1,0,0,1,0,1,0],
'_':[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1],
'/':[0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0],
',':[0,0,0,0,0,0,0,0,1,1,0,1],'!':[1,1,0,1,1,0,1,1,0,0,0,0,1,1,0],
'?':[0,1,1,1,0,1,0,0,0,1,0,0,1,1,0,0,0,0,0,0,0,0,1,0,0],
'0':[0,1,1,1,0,1,0,0,1,1,1,0,1,0,1,1,1,0,0,1,0,1,1,1,0],
'1':[0,1,1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0,0,1,1,1,1,0],
'2':[0,1,1,1,0,1,0,0,0,1,0,0,1,1,0,0,1,0,0,0,1,1,1,1,1],
'3':[1,1,1,1,0,0,0,0,0,1,0,1,1,1,0,0,0,0,0,1,1,1,1,1,0],
'4':[0,0,0,1,0,0,0,1,1,0,0,1,0,1,0,1,1,1,1,1,0,0,0,1,0],
'5':[1,1,1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,0,1,1,1,1,1,0],
'6':[0,1,1,1,0,1,0,0,0,0,1,1,1,1,0,1,0,0,0,1,0,1,1,1,0],
'7':[1,1,1,1,1,0,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0],
'8':[0,1,1,1,0,1,0,0,0,1,0,1,1,1,0,1,0,0,0,1,0,1,1,1,0],
'9':[0,1,1,1,0,1,0,0,0,1,0,1,1,1,1,0,0,0,0,1,0,1,1,1,0],
}

def draw_text(pixels, w, x, y, text, color, scale=1):
    cx, cy, s = x, y, scale
    for ch in text:
        if ch not in FONT: cx += 6*s; continue
        bm = FONT[ch]
        rows = len(bm)//5 if len(bm)%5==0 else 7
        cols = len(bm)//rows
        for row in range(rows):
            for col in range(cols):
                if bm[row*cols+col]:
                    for dy in range(s):
                        for dx in range(s):
                            px,py=cx+col*s+dx,cy+row*s+dy
                            if 0<=px<w and 0<=py<len(pixels)//w:
                                pixels[py*w+px]=color
        cx+=(cols+1)*s

def gen_preview(theme_path, out_path):
    with open(theme_path) as f:
        t = json.load(f)
    c = t['colors']
    W, H = 600, 380
    pixels = [(30,30,30)] * (W * H)

    bg = hex_to_rgb(c['windowBg'])
    menu = hex_to_rgb(c['menuBarBg'])
    text = hex_to_rgb(c['text'])
    title = hex_to_rgb(c['titleBg'])
    border_c = hex_to_rgb(c['border'])
    user_bg = hex_to_rgb(c['bubbleUserBg'])
    asst_bg = hex_to_rgb(c['bubbleAssistantBg'])
    tool_bg = hex_to_rgb(c['toolBg'])
    tool_title = hex_to_rgb(c['toolTitleColor'])
    tool_text = hex_to_rgb(c['toolResultText'])
    md_h1 = hex_to_rgb(c['mdH1'])
    md_code = hex_to_rgb(c['mdCode'])
    md_code_bg = hex_to_rgb(c['mdCodeBg'])
    reason_card = hex_to_rgb(c['reasoningCardBg'])
    reason_border = hex_to_rgb(c['reasoningBorder'])
    reason_text = hex_to_rgb(c['reasoningTextColor'])
    phase_idle = hex_to_rgb(c['phaseIdle'])
    todo_title = hex_to_rgb(c['todoTitle'])
    todo_done = hex_to_rgb(c['todoDone'])
    todo_prog = hex_to_rgb(c['todoInProgress'])
    sep = hex_to_rgb(c['separator'])

    # Window bg
    rect(pixels, W, 0, 0, W, H, bg)
    # Title bar
    rect(pixels, W, 0, 0, W, 28, title)
    draw_text(pixels, W, 10, 5, "proJV  |  " + t['name'], text, 2)
    # Menu bar
    rect(pixels, W, 0, 28, W, 22, menu)
    draw_text(pixels, W, 8, 32, "File  Settings  View  Help", text, 1)
    # Left border
    rect(pixels, W, 0, 50, 2, H-70, border_c)

    # User bubble (right side)
    rounded_rect(pixels, W, 280, 60, 306, 36, user_bg, 6)
    draw_text(pixels, W, 290, 68, "User: What is proJV?", text, 1)

    # Assistant bubble (left)
    rounded_rect(pixels, W, 8, 106, 578, 110, asst_bg, 6)
    draw_text(pixels, W, 18, 114, "Assistant: A minimal AI coding agent.", text, 1)
    draw_text(pixels, W, 18, 132, "## Features", md_h1, 1)
    rounded_rect(pixels, W, 18, 150, 80, 16, md_code_bg, 3)
    draw_text(pixels, W, 20, 152, "read_file", md_code, 1)
    draw_text(pixels, W, 108, 152, "  inspect source code", text, 1)
    rounded_rect(pixels, W, 18, 172, 80, 16, md_code_bg, 3)
    draw_text(pixels, W, 20, 174, "exec_shell", md_code, 1)
    draw_text(pixels, W, 108, 174, "  compile & run", text, 1)

    # Reasoning card (bottom of assistant area)
    rounded_rect(pixels, W, 18, 196, 270, 14, reason_card, 3)
    rect(pixels, W, 18, 196, 2, 14, reason_border)
    draw_text(pixels, W, 24, 198, "[+] Reasoning (collapsed)", reason_text, 1)

    # Tool call bubble
    rounded_rect(pixels, W, 8, 225, 578, 50, tool_bg, 6)
    draw_text(pixels, W, 18, 233, "grep_files: theme", tool_title, 1)
    rect(pixels, W, 18, 248, 558, 1, sep)
    draw_text(pixels, W, 18, 255, "Found 12 matches in src/ui/theme.cpp", tool_text, 1)

    # TODO panel area (right side)
    rect(pixels, W, 400, 285, 186, 44, border_c)
    rect(pixels, W, 401, 286, 184, 42, bg)
    draw_text(pixels, W, 408, 290, "TODO", todo_title, 1)
    draw_text(pixels, W, 408, 305, "[x] Add theme support", todo_done, 1)
    draw_text(pixels, W, 408, 318, "[*] Write docs", todo_prog, 1)

    # Status bar
    rect(pixels, W, 0, H-20, W, 20, menu)
    rect(pixels, W, 0, H-21, W, 1, border_c)
    draw_text(pixels, W, 8, H-16, "deepseek-chat | tok: 1.2K+0.8K | msgs: 12 | Idle", phase_idle, 1)

    png = make_png(W, H, pixels)
    with open(out_path, 'wb') as f:
        f.write(png)
    print(f"  {out_path}")

def main():
    theme_dir = os.path.dirname(os.path.abspath(__file__))
    preview_dir = os.path.join(theme_dir, 'previews')
    os.makedirs(preview_dir, exist_ok=True)

    for fn in sorted(os.listdir(theme_dir)):
        if fn.endswith('.json'):
            name = fn.replace('.json', '')
            gen_preview(os.path.join(theme_dir, fn),
                       os.path.join(preview_dir, f'{name}.png'))
    cnt = len([f for f in os.listdir(preview_dir) if f.endswith('.png')])
    print(f"\nDone -- {cnt} previews in {preview_dir}")

if __name__ == '__main__':
    main()
