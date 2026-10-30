# 生成程序图标 icon.ico（深色圆角底 + 白色表盘 + 蓝色指针）
from PIL import Image, ImageDraw
import os

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.rounded_rectangle((8, 8, S - 8, S - 8), radius=56, fill=(16, 20, 28, 255))
c = S // 2
R = 86
d.ellipse((c - R, c - R, c + R, c + R), outline=(240, 244, 248, 255), width=18)
d.line((c, c, c, c - 58), fill=(240, 244, 248, 255), width=18)
d.line((c, c, c + 46, c + 22), fill=(0, 175, 255, 255), width=18)
d.ellipse((c - 14, c - 14, c + 14, c + 14), fill=(0, 175, 255, 255))

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.ico")
img.save(out, sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (48, 48), (64, 64)])
print(out, os.path.getsize(out))
