#!/usr/bin/env python3
"""Qt 6 window for exercising wlpuppet; interactions are printed to stdout."""

import sys

from PyQt6.QtWidgets import QApplication, QLineEdit, QMenu, QPushButton, QVBoxLayout, QWidget

app = QApplication(sys.argv)
window = QWidget()
window.setWindowTitle("wlpuppet qt probe")
window.resize(400, 200)
layout = QVBoxLayout(window)

clicks = 0


def clicked():
    global clicks
    clicks += 1
    print(f"clicked {clicks}", flush=True)


button = QPushButton("Click me")
button.clicked.connect(clicked)
layout.addWidget(button)

menu = QMenu(window)
for name in ("Alpha", "Beta"):
    menu.addAction(name).triggered.connect(lambda _checked, n=name: print(f"menu {n}", flush=True))
menu_button = QPushButton("Menu")
menu_button.setMenu(menu)
layout.addWidget(menu_button)

entry = QLineEdit()
entry.textChanged.connect(lambda text: print(f"text {text!r}", flush=True))
layout.addWidget(entry)

window.show()
sys.exit(app.exec())
