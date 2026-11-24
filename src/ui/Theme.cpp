#include "ui/Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace rd::theme {

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, kBg);
    pal.setColor(QPalette::WindowText, kText);
    pal.setColor(QPalette::Base, kPanel2);
    pal.setColor(QPalette::AlternateBase, kPanel);
    pal.setColor(QPalette::Text, kText);
    pal.setColor(QPalette::Button, kPanel2);
    pal.setColor(QPalette::ButtonText, kText);
    pal.setColor(QPalette::Highlight, QColor(0x55, 0x55, 0x55));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, QColor(0x33, 0x33, 0x33));
    pal.setColor(QPalette::ToolTipText, kText);
    app.setPalette(pal);
    QFont f("Yu Gothic UI");
    f.setPointSizeF(9);
    app.setFont(f);
    app.setStyleSheet(R"(
QWidget { background: #1e1e1e; color: #d6d6d6; }
QMainWindow::separator { background: #3a3a3a; width: 3px; height: 3px; }
QDockWidget::title { background: #262626; padding: 4px 8px; color: #8c8c8c; }
QScrollArea, QScrollArea > QWidget > QWidget { background: #262626; }
QPushButton { background: #2e2e2e; border: 1px solid #3a3a3a; border-radius: 4px; padding: 4px 10px; }
QPushButton:hover { border-color: #555; }
QPushButton:pressed, QPushButton:checked { background: #3b3b3b; border-color: #d28b2c; }
QPushButton#Accent { background: #d28b2c; color: #111; border: none; font-weight: 600; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px 6px; }
QToolButton:hover { border-color: #555; }
QToolButton:checked { border-color: #d28b2c; background: #333; }
QComboBox, QLineEdit, QSpinBox { background: #2e2e2e; border: 1px solid #3a3a3a; border-radius: 3px; padding: 2px 6px; }
QComboBox QAbstractItemView { background: #2e2e2e; selection-background-color: #444; }
QTabBar::tab { background: #262626; padding: 4px 10px; border: none; color: #8c8c8c; }
QTabBar::tab:selected { color: #d6d6d6; border-bottom: 2px solid #d28b2c; }
QTabWidget::pane { border: none; }
QListView, QListWidget { background: #1e1e1e; border: none; }
QListWidget::item:selected { background: #3a3a3a; }
QScrollBar:vertical { background: #262626; width: 10px; }
QScrollBar::handle:vertical { background: #444; border-radius: 4px; min-height: 24px; }
QScrollBar:horizontal { background: #262626; height: 10px; }
QScrollBar::handle:horizontal { background: #444; border-radius: 4px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QMenu { background: #2e2e2e; border: 1px solid #3a3a3a; }
QMenu::item:selected { background: #444; }
QStatusBar { background: #262626; color: #8c8c8c; }
QToolTip { background: #333; color: #d6d6d6; border: 1px solid #555; }
)");
}

}  // namespace rd::theme
