import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


W, H = 480, 800


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = [
        "C:/Windows/Fonts/Arialbd.ttf" if bold else "C:/Windows/Fonts/Arial.ttf",
        "C:/Windows/Fonts/segoeuib.ttf" if bold else "C:/Windows/Fonts/segoeui.ttf",
    ]
    for path in candidates:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


F_TITLE = font(38, True)
F_H2 = font(28, True)
F_BODY = font(25)
F_BODY_B = font(25, True)
F_META = font(20)
F_META_B = font(20, True)
F_TASK = font(28, True)
F_SMALL = font(22)


def text(draw: ImageDraw.ImageDraw, xy: tuple[int, int], value: str, fnt: ImageFont.ImageFont, fill=0) -> None:
    draw.text(xy, value, font=fnt, fill=fill)


def rounded(draw: ImageDraw.ImageDraw, box, radius=8, outline=0, width=1, fill=None) -> None:
    draw.rounded_rectangle(box, radius=radius, outline=outline, width=width, fill=fill)


def divider(draw: ImageDraw.ImageDraw, y: int) -> None:
    draw.line((48, y, 432, y), fill=0, width=2)


def progress(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, pct: int, hatch=False) -> None:
    rounded(draw, (x, y, x + w, y + 20), 7, 0, 2, None)
    fill_w = int((w - 6) * pct / 100)
    fill_box = (x + 3, y + 3, x + 3 + fill_w, y + 17)
    draw.rounded_rectangle(fill_box, radius=5, fill=0)
    if hatch:
        for xx in range(x + 3, x + 3 + fill_w + 18, 12):
            draw.line((xx - 14, y + 17, xx, y + 3), fill=255, width=2)


def cube(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    draw.rectangle((x + 8, y, x + 26, y + 18), outline=0, width=1)
    draw.line((x + 8, y, x + 17, y - 7, x + 35, y - 7, x + 26, y, x + 26, y + 18, x + 35, y + 11, x + 35, y - 7), fill=0, width=1)
    draw.line((x + 17, y + 8, x + 35, y - 7), fill=0, width=1)


def window_icon(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    rounded(draw, (x, y, x + 30, y + 30), 8, 0, 3)
    draw.rectangle((x + 8, y + 8, x + 22, y + 22), outline=0, width=2)


def clock(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    rounded(draw, (x, y, x + 30, y + 30), 15, 0, 3)
    draw.line((x + 15, y + 8, x + 15, y + 16, x + 22, y + 20), fill=0, width=2)


def tasks_icon(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    for i in range(3):
        yy = y + i * 10
        draw.line((x, yy, x + 5, yy + 4, x + 10, yy - 4), fill=0, width=2)
        draw.line((x + 18, yy, x + 38, yy), fill=0, width=2)


def token_icon(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    draw.rectangle((x + 3, y + 5, x + 27, y + 25), outline=0, width=3)
    draw.line((x + 9, y + 11, x + 21, y + 11), fill=0, width=2)
    draw.line((x + 9, y + 17, x + 21, y + 17), fill=0, width=2)


def check_badge(draw: ImageDraw.ImageDraw, x: int, y: int) -> None:
    rounded(draw, (x, y, x + 48, y + 48), 24, 0, 3)
    draw.line((x + 12, y + 26, x + 21, y + 35, x + 37, y + 15), fill=0, width=3)


def render() -> Image.Image:
    img = Image.new("L", (W, H), 255)
    draw = ImageDraw.Draw(img)

    rounded(draw, (18, 18, 462, 782), 8, 0, 2)
    rounded(draw, (25, 25, 455, 775), 5, 0, 1)

    text(draw, (58, 58), "Codex Status", F_TITLE)
    cube(draw, 394, 62)
    text(draw, (60, 112), "gpt-5.5 high fast", F_META_B)
    text(draw, (60, 138), "MatrixSpec · main · Ready", F_META)
    divider(draw, 168)

    window_icon(draw, 58, 192)
    text(draw, (104, 192), "Context window", F_H2)
    text(draw, (58, 240), "79% used", F_BODY_B)
    progress(draw, 58, 274, 364, 79, True)
    divider(draw, 318)

    token_icon(draw, 58, 342)
    text(draw, (104, 342), "Token usage", F_H2)
    text(draw, (58, 390), "1.46M total used", F_BODY_B)
    divider(draw, 430)

    clock(draw, 58, 454)
    text(draw, (104, 454), "Quota usage", F_H2)
    text(draw, (58, 502), "5h", F_BODY_B)
    text(draw, (106, 502), "97%", F_BODY)
    progress(draw, 58, 536, 364, 97)
    text(draw, (58, 584), "weekly", F_BODY_B)
    text(draw, (156, 584), "93%", F_BODY)
    progress(draw, 58, 618, 364, 93, True)
    divider(draw, 662)

    tasks_icon(draw, 58, 686)
    text(draw, (104, 686), "Tasks", F_TASK)
    text(draw, (58, 734), "4/4", F_TASK)
    text(draw, (116, 742), "Goal achieved (53m)", F_SMALL)
    check_badge(draw, 366, 694)
    return img


def main() -> None:
    parser = argparse.ArgumentParser(description="Render the XTEINK status screen locally without flashing.")
    parser.add_argument("--out", default="local-status-preview.png")
    args = parser.parse_args()
    out = Path(args.out)
    img = render()
    img.save(out)
    print(f"Saved {out} ({W}x{H})")


if __name__ == "__main__":
    main()
