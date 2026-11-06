#!/usr/bin/env python3
"""生成 MiniPrinter 的 launcher 图标（纯标准库，不需要 Pillow）。

思路：先画一张 512x512 的 RGBA 位图（内部 2x 超采样做抗锯齿），
再盒式降采样到 Android 需要的各档 mipmap 尺寸，最后用 zlib 写 PNG。

图形：深色圆角底 + 浅灰机身 + 白色纸条 + 青色指示灯。
"""
import zlib, struct, pathlib, os

BG    = (27, 36, 48)        # 深蓝灰底
BODY  = (232, 237, 242)     # 机身浅灰
SLOT  = (14, 20, 27)        # 出纸口深色
PAPER = (255, 255, 255)     # 纸条白
TEXT  = (184, 198, 212)     # 纸条上的文字线
LED   = (79, 195, 247)      # 指示灯青

S = 512          # 目标尺寸
SS = 2           # 超采样倍数
W = S * SS


def rounded_rect(px, x0, y0, x1, y1, r, color):
    """往 px（W x W 的 4 元组列表）里填一个圆角矩形，带抗锯齿（靠超采样）"""
    n = len(px)
    x0, y0 = max(0, int(x0)), max(0, int(y0))
    x1, y1 = min(n - 1, int(x1)), min(n - 1, int(y1))
    r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            # 圆角判定
            dx = dy = 0.0
            if x < x0 + r:
                dx = x0 + r - x
            elif x > x1 - r:
                dx = x - (x1 - r)
            if y < y0 + r:
                dy = y0 + r - y
            elif y > y1 - r:
                dy = y - (y1 - r)
            if dx * dx + dy * dy <= r * r:
                px[y][x] = color


def circle(px, cx, cy, rad, color):
    for y in range(int(cy - rad), int(cy + rad) + 1):
        dy = y - cy
        for x in range(int(cx - rad), int(cx + rad) + 1):
            dx = x - cx
            if dx * dx + dy * dy <= rad * rad:
                px[y][x] = color


def build():
    px = [[BG for _ in range(W)] for _ in range(W)]
    sc = SS

    def R(x0, y0, x1, y1, r, c):
        rounded_rect(px, x0 * sc, y0 * sc, x1 * sc, y1 * sc, r * sc, c)

    def C(cx, cy, rad, c):
        circle(px, cx * sc, cy * sc, rad * sc, c)

    # 1) 底：整块深色，外圈再切圆角（用背景色覆盖四角外区域）
    R(0, 0, 512, 512, 96, BG)

    # 2) 纸条（先画，会被机身压住下半截）
    R(112, 92, 400, 252, 10, PAPER)

    # 3) 纸条上的三行"字"
    R(148, 128, 332, 142, 7, TEXT)
    R(148, 162, 366, 176, 7, TEXT)
    R(148, 196, 292, 210, 7, TEXT)

    # 4) 机身（盖住纸条下半）
    R(80, 232, 432, 400, 28, BODY)

    # 5) 出纸口：机身顶部一条深色缝
    R(112, 232, 400, 264, 8, SLOT)

    # 6) 指示灯
    C(392, 356, 16, LED)

    # 降采样（盒式平均）
    out = []
    for y in range(S):
        row = []
        for x in range(S):
            r = g = b = a = 0
            for dy in range(SS):
                for dx in range(SS):
                    p = px[y * SS + dy][x * SS + dx]
                    r += p[0]; g += p[1]; b += p[2]; a += 255
            n = SS * SS
            row.append((r // n, g // n, b // n, a // n))
        out.append(row)
    return out


def write_png(path, pixels):
    h = len(pixels); w = len(pixels[0])
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in pixels)
    comp = zlib.compress(raw, 9)

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
           + chunk(b'IDAT', comp)
           + chunk(b'IEND', b''))
    pathlib.Path(path).write_bytes(png)
    return len(png)


def resize(pixels, size):
    """从 S x S 盒式缩到 size x size"""
    src = len(pixels); step = src / size
    out = []
    for y in range(size):
        y0, y1 = int(y * step), max(int((y + 1) * step), int(y * step) + 1)
        row = []
        for x in range(size):
            x0, x1 = int(x * step), max(int((x + 1) * step), int(x * step) + 1)
            r = g = b = a = n = 0
            for yy in range(y0, min(y1, src)):
                for xx in range(x0, min(x1, src)):
                    p = pixels[yy][xx]
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]; n += 1
            row.append((r // n, g // n, b // n, a // n) if n else (0, 0, 0, 0))
        out.append(row)
    return out


if __name__ == '__main__':
    base = pathlib.Path('app/src/main/res')
    px = build()
    sizes = {'mdpi': 48, 'hdpi': 72, 'xhdpi': 96, 'xxhdpi': 144, 'xxxhdpi': 192}
    for name, sz in sizes.items():
        d = base / f'mipmap-{name}'
        d.mkdir(parents=True, exist_ok=True)
        n = write_png(d / 'ic_launcher.png', resize(px, sz))
        write_png(d / 'ic_launcher_round.png', resize(px, sz))
        print(f'  mipmap-{name:<8} {sz}x{sz}  {n} B')

    # 512 预览图，方便看效果 / 上传商店
    prev = base / 'drawable-nodpi'
    prev.mkdir(parents=True, exist_ok=True)
    n = write_png(prev / 'ic_launcher_preview.png', px)
    print(f'  preview        512x512  {n} B')

    # Android 8+ 自适应图标的前景层：内容缩到 2/3 居中，四周留安全边距，
    # 这样被裁成圆形/圆角时机身两端不会被切掉
    inner = resize(px, int(S * 2 / 3))
    fg = [[(0, 0, 0, 0)] * S for _ in range(S)]
    off = (S - int(S * 2 / 3)) // 2
    for y in range(len(inner)):
        for x in range(len(inner[y])):
            fg[off + y][off + x] = inner[y][x]
    n = write_png(prev / 'ic_launcher_foreground.png', fg)
    print(f'  foreground     512x512  {n} B')
