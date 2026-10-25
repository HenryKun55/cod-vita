"""LiveArea pictures from the user's copy of the game (not kept in the repository):
the main menu background (ui/assets/main_back.tga in main/pak0.pk3) for the boot
picture and the background, its Call of Duty logo corner for the icon.

usage: make_vita_livearea.py GAME_DIR SCE_SYS_DIR
"""
import io
import os
import sys
import zipfile

from PIL import Image


def main():
    game, out = sys.argv[1], sys.argv[2]
    with zipfile.ZipFile(os.path.join(game, 'main', 'pak0.pk3')) as z:
        art = Image.open(io.BytesIO(z.read('ui/assets/main_back.tga'))).convert('RGB')
    os.makedirs(os.path.join(out, 'livearea', 'contents'), exist_ok=True)
    w, h = art.size
    # 16:9 crop of the 4:3 art, from the top (the logo and the gun).
    ch = w * 9 // 16
    wide = art.crop((0, 0, w, min(h, ch)))
    wide.resize((960, 544), Image.LANCZOS).save(os.path.join(out, 'pic0.png'))
    wide.resize((840, 500), Image.LANCZOS).save(os.path.join(out, 'livearea', 'contents', 'bg.png'))
    wide.resize((280, 158), Image.LANCZOS).save(os.path.join(out, 'livearea', 'contents', 'startup.png'))
    # Icon: the right part of the art (the logo), square.
    side = h // 2
    icon = art.crop((w - side - w // 40, h // 12, w - w // 40, h // 12 + side))
    icon.resize((128, 128), Image.LANCZOS).convert('P', palette=Image.ADAPTIVE).save(os.path.join(out, 'icon0.png'))


if __name__ == '__main__':
    main()
