"""RawDeveloper の起動。"""
import os
import sys


def main():
    os.environ.setdefault("QT_ENABLE_HIGHDPI_SCALING", "1")
    from PySide6.QtCore import QCoreApplication, Qt
    from PySide6.QtWidgets import QApplication
    QCoreApplication.setOrganizationName("RawDeveloper")
    QCoreApplication.setApplicationName("RawDeveloper")
    app = QApplication(sys.argv)
    from .ui import theme
    theme.apply(app)
    from .ui.main_window import MainWindow
    w = MainWindow()
    if len(sys.argv) > 1 and os.path.isdir(sys.argv[1]):
        w.open_folder(sys.argv[1])
    w.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
