"""ダークグレー基調のテーマ（写真の色を正しく見るため、彩度のない灰色）。"""
from PySide6.QtGui import QColor, QPalette, QFont
from PySide6.QtWidgets import QApplication, QStyleFactory

BG = "#1e1e1e"
PANEL = "#262626"
PANEL2 = "#2e2e2e"
LINE = "#3a3a3a"
TEXT = "#d6d6d6"
TEXT_DIM = "#8c8c8c"
ACCENT = "#d28b2c"       # Resolve 風のオレンジ

STYLE = f"""
QWidget {{ background: {BG}; color: {TEXT}; font-size: 9pt; }}
QMainWindow::separator {{ background: {LINE}; width: 3px; height: 3px; }}
QDockWidget {{ titlebar-close-icon: none; }}
QDockWidget::title {{ background: {PANEL}; padding: 4px 8px; color: {TEXT_DIM}; }}
QScrollArea, QScrollArea > QWidget > QWidget {{ background: {PANEL}; }}
QFrame#Section {{ background: {PANEL}; }}
QLabel#SectionTitle {{ color: {TEXT}; font-weight: 600; }}
QLabel.dim, QLabel#Dim {{ color: {TEXT_DIM}; }}
QPushButton {{ background: {PANEL2}; border: 1px solid {LINE}; border-radius: 4px; padding: 4px 10px; }}
QPushButton:hover {{ border-color: #555; }}
QPushButton:pressed, QPushButton:checked {{ background: #3b3b3b; border-color: {ACCENT}; }}
QPushButton#Accent {{ background: {ACCENT}; color: #111; border: none; font-weight: 600; }}
QToolButton {{ background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px; }}
QToolButton:hover {{ border-color: #555; }}
QToolButton:checked {{ border-color: {ACCENT}; background: #333; }}
QComboBox, QLineEdit, QSpinBox {{ background: {PANEL2}; border: 1px solid {LINE}; border-radius: 3px; padding: 2px 6px; }}
QComboBox QAbstractItemView {{ background: {PANEL2}; selection-background-color: #444; }}
QTabBar::tab {{ background: {PANEL}; padding: 4px 10px; border: none; color: {TEXT_DIM}; }}
QTabBar::tab:selected {{ color: {TEXT}; border-bottom: 2px solid {ACCENT}; }}
QTabWidget::pane {{ border: none; }}
QListView, QTreeView, QListWidget {{ background: {BG}; border: none; }}
QListWidget::item:selected, QTreeView::item:selected {{ background: #3a3a3a; }}
QScrollBar:vertical {{ background: {PANEL}; width: 10px; }}
QScrollBar::handle:vertical {{ background: #444; border-radius: 4px; min-height: 24px; }}
QScrollBar:horizontal {{ background: {PANEL}; height: 10px; }}
QScrollBar::handle:horizontal {{ background: #444; border-radius: 4px; min-width: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ width: 0; height: 0; }}
QMenu {{ background: {PANEL2}; border: 1px solid {LINE}; }}
QMenu::item:selected {{ background: #444; }}
QStatusBar {{ background: {PANEL}; color: {TEXT_DIM}; }}
QCheckBox::indicator {{ width: 13px; height: 13px; }}
QToolTip {{ background: #333; color: {TEXT}; border: 1px solid #555; }}
QProgressBar {{ background: {PANEL2}; border: 1px solid {LINE}; border-radius: 3px; text-align: center; }}
QProgressBar::chunk {{ background: {ACCENT}; }}
"""


def apply(app: QApplication):
    app.setStyle(QStyleFactory.create("Fusion"))
    pal = QPalette()
    for role, c in [
        (QPalette.Window, BG), (QPalette.WindowText, TEXT), (QPalette.Base, PANEL2),
        (QPalette.AlternateBase, PANEL), (QPalette.Text, TEXT), (QPalette.Button, PANEL2),
        (QPalette.ButtonText, TEXT), (QPalette.Highlight, "#555555"), (QPalette.HighlightedText, "#ffffff"),
        (QPalette.ToolTipBase, "#333333"), (QPalette.ToolTipText, TEXT),
    ]:
        pal.setColor(role, QColor(c))
    app.setPalette(pal)
    f = QFont("Yu Gothic UI")
    f.setPointSizeF(9)
    app.setFont(f)
    app.setStyleSheet(STYLE)
