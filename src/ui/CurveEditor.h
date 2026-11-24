#pragma once
// トーンカーブ（Y / R / G / B）
//  クリック：点を追加　ドラッグ：移動　右クリック：点を削除　ダブルクリック：このチャンネルをリセット
//  Shift＋クリック：形を変えずに点を追加
#include <QWidget>
#include <array>
#include <vector>

#include "engine/Params.h"

class QButtonGroup;

namespace rd {

class CurveCanvas : public QWidget {
    Q_OBJECT
public:
    explicit CurveCanvas(QWidget* parent = nullptr);
    void setCurves(const std::array<CurvePoints, 4>& c);
    void setHistogram(const std::vector<float>& h);
    void setChannel(int ch);
    const std::array<CurvePoints, 4>& curves() const { return curves_; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

signals:
    void changed(const std::array<CurvePoints, 4>& c, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF plotRect() const;
    QPointF toPx(double x, double y) const;
    QPointF fromPx(const QPointF& p) const;
    int nearest(const QPointF& p) const;
    std::array<CurvePoints, 4> curves_;
    std::vector<float> hist_;
    int ch_ = 0;
    int drag_ = -1;
    bool hover_ = false;
    QPointF hoverVal_;
};

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    CurveCanvas* canvas() { return canvas_; }

private:
    CurveCanvas* canvas_;
    QButtonGroup* group_;
};

}  // namespace rd
