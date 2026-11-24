#pragma once
// 写真ビューアー
//  ズーム：ホイール（カーソル位置中心）、Z／ダブルクリックで全体⇔等倍
//  パン：中ボタン・Space＋ドラッグ、ズーム中の左ドラッグ
//  比較：ワイプ（左右／上下）、並べて比較（左右／上下／自動）。並べたときはズームと位置が連動
//  重ね表示：白飛び・黒つぶれ警告、ヒストグラムでなぞった範囲
//  切り抜きツール：枠のドラッグ、比率固定、三分割ガイド
#include <QImage>
#include <QWidget>
#include <array>
#include <optional>

namespace rd {

enum class CompareMode { Off, WipeV, WipeH, SideH, SideV, SideAuto };

struct CropRect {
    double x = 0, y = 0, w = 1, h = 1;
};

class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(QWidget* parent = nullptr);
    // fullSize：元画像（切り抜き後）の画素数。ズーム 100% の基準
    void setImage(const QImage& img, QSize fullSize, bool hires = false);
    void resetView();                                   // 全体表示に戻す（別の写真を開いたとき）
    // 拡大中に、見えている範囲だけフル解像度で重ねて表示する
    void setRoi(const QImage& img, const QRect& rectFull);
    void clearRoi() { roi_ = QImage(); update(); }
    QRect visibleFullRect() const;                      // 補正後の画像で見えている範囲（元画像の画素）
    void setBefore(const QImage& img) { before_ = img; update(); }
    void clear(const QString& message);
    void setMessage(const QString& m) { message_ = m; update(); }
    void setCompare(CompareMode m);
    CompareMode compare() const { return compare_; }
    void setLinked(bool on) { linked_ = on; offsets_[1] = offsets_[0]; update(); }
    void setBypass(bool on) { bypass_ = on; update(); }
    void setClipOverlay(bool on) { clip_ = on; clipCache_ = QImage(); update(); }
    void setMaskRange(int lo, int hi);
    void setBeforeLabel(const QString& s) { beforeLabel_ = s; update(); }
    void swapSides() { swapped_ = !swapped_; update(); }
    void setPicking(bool on) { picking_ = on; setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor); }
    void zoomFit();
    void zoom100();
    void toggleZoom();
    bool isFit() const { return fit_; }
    double zoom() const { return zoom_; }
    double displayScaleOfImage() const;  // 表示画像の画素 / 元画像の画素
    // 切り抜き
    void setCropMode(bool on, const CropRect& r = {});
    bool cropMode() const { return cropMode_; }
    CropRect cropRect() const { return crop_; }
    void setCropAspect(double a) { aspect_ = a; }  // 0 = 自由（幅/高さ）

signals:
    void probe(std::optional<std::array<int, 3>> rgb);
    void wantHiRes();
    void picked(int x, int y);              // 表示画像上の画素（WB スポイト）
    void cropChanged(const rd::CropRect& r, bool final);
    void viewChanged();                                 // ズーム・移動・比較方法が変わった

protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    std::array<QRectF, 2> panes() const;  // 並べて比較のときは 2 つ、それ以外は [0] だけ
    bool sideBySide() const;
    bool sideHorizontal() const;
    QRectF imageRect(const QRectF& pane, int idx) const;
    double fitZoom() const;
    void setZoom(double z, QPointF anchor, bool fit);
    void clamp();
    int paneAt(const QPointF& p) const;
    QImage clipOverlay();
    void drawCrop(class QPainter& p, const QRectF& r);
    QRectF cropPx(const QRectF& r) const;
    void dragCrop(const QPointF& pos, const QRectF& r);

    QImage img_, before_, mask_, clipCache_, roi_;
    QRect roiRect_;
    QSize full_{1, 1};
    bool hires_ = false;
    QString message_, beforeLabel_ = QStringLiteral("補正前");
    double zoom_ = 1;
    bool fit_ = true;
    std::array<QPointF, 2> offsets_{};
    bool linked_ = true, swapped_ = false;
    CompareMode compare_ = CompareMode::Off;
    double split_ = 0.5;
    bool bypass_ = false, clip_ = false, picking_ = false;
    // 操作中
    int panPane_ = -1;
    QPointF panStart_, offStart_;
    bool space_ = false, splitDrag_ = false;
    // 切り抜き
    bool cropMode_ = false;
    CropRect crop_, cropStart_;
    QString cropHandle_;
    QPointF cropP0_;
    double aspect_ = 0;
};

}  // namespace rd
