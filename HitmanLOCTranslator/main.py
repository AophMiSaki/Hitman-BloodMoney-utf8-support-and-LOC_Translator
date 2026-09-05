# -*- coding: utf-8 -*-
"""進入點：python main.py 啟動翻譯工具 GUI。

其餘模組（原本平放在本資料夾）已移到 lib 子資料夾底下，這裡先把該
路徑掛到 sys.path，模組間仍用扁平匯入（import db as dbmod ...）不需改。
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))

from gui import App

if __name__ == "__main__":
    App().mainloop()
