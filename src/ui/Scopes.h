#pragma once
// ヒストグラムとスコープ（Resolve 風）：ヒストグラム（RGB/輝度/RGB 別）、波形、RGB パレード、ベクトルスコープ
#include <QWidget>
#include <array>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QGridLayout;
class QLabel;

namespace rd {

struct ScopeData {
    int w = 0, h = 0;                       // 縮小後
    std::vector<std::uint8_t> rgb;          // 縮小した画像
    std::vector<int> luma;
    std::array<std::vector<float>, 3> hist; // R, G, B
    std::vector<float> histL;
    double clipHi = 0, clipLo = 0;
    std::vector<float> wave;                // 256 x 256
    std::array<std::vector<float>, 3> parade;  // 256 x 160
    std::vector<float> vec;                 // 200 x 200
    static std::shared_ptr<ScopeData> make(const std::uint8_t* rgb8, int w, int h);
};

enum class ScopeMode { HistRgb, HistLuma, HistSplit, Waveform, Parade, Vector };

class ScopeView : public QWidget {
    Q_OBJECT
public:
    explicit ScopeView(ScopeMode mode, QWidget* parent = nullptr);
    void setData(std::shared_ptr<ScopeData> d) { data_ = std::move(d); update(); }
    void setGhost(std::shared_ptr<ScopeData> d) { ghost_ = std::move(d); update(); }
    std::shared_ptr<ScopeData> data() const { return data_; }
    void setZoneHighlight(const QString& key) { zoneHi_ = key; update(); }
    void setProbe(std::optional<std::array<int, 3>> p) { probe_ = p; update(); }
    void setMode(ScopeMode m);
    ScopeMode mode() const { return mode_; }

signals:
    void hoverRange(int lo, int hi);    // lo<0 で解除
    void zoneDrag(const QString& key, double dv, bool final);
    void modeChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF plotRect() const;
    void paintHist(class QPainter& p);
    void paintWave(class QPainter& p);
    void paintParade(class QPainter& p);
    void paintVector(class QPainter& p);
    void paintGrid(class QPainter& p, const QRectF& r);
    ScopeMode mode_;
    std::shared_ptr<ScopeData> data_, ghost_;
    QString zoneHi_;
    std::optional<std::array<int, 3>> probe_;
    double hoverX_ = -1;
    QString dragKey_;
    double dragX_ = 0;
    bool log_ = false;
};

class ScopesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ScopesPanel(QWidget* parent = nullptr);
    std::shared_ptr<ScopeData> setImage(const std::uint8_t* rgb8, int w, int h);
    void beginGhost();
    void endGhost();
    void setZoneHighlight(const QString& key);
    void setProbe(std::optional<std::array<int, 3>> p);

signals:
    void hoverRange(int lo, int hi);
    void zoneDrag(const QString& key, double dv, bool final);

private:
    void relayout();
    void save();
    QComboBox* layoutCombo_;
    QComboBox* firstCombo_;
    QGridLayout* grid_;
    std::vector<ScopeView*> views_;
    bool ghosting_ = false;
};

}  // namespace rd
