#pragma once
#include <QImage>
#include <QWidget>

namespace rd {

// 写真ビューアー（この段階では全体表示とホイールズームのみ）
class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    void setMessage(const QString& msg);

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    QRectF imageRect() const;
    QImage img_;
    QString message_;
    double zoom_ = 0.0;  // 0 = 画面に合わせる
    QPointF offset_;
    QPointF dragStart_, offsetStart_;
};

}  // namespace rd
