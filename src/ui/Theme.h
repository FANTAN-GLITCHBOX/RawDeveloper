#pragma once
#include <QColor>
#include <QString>

class QApplication;

namespace rd::theme {

inline const QColor kBg{0x1e, 0x1e, 0x1e};
inline const QColor kPanel{0x26, 0x26, 0x26};
inline const QColor kPanel2{0x2e, 0x2e, 0x2e};
inline const QColor kLine{0x3a, 0x3a, 0x3a};
inline const QColor kText{0xd6, 0xd6, 0xd6};
inline const QColor kTextDim{0x8c, 0x8c, 0x8c};
inline const QColor kAccent{0xd2, 0x8b, 0x2c};  // Resolve 風のオレンジ

void apply(QApplication& app);

}  // namespace rd::theme
