#include "ui/DevelopPanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QListWidget>
#include <QMap>
#include <QTimer>
#include <QLineEdit>
#include <QGridLayout>
#include <QStandardItemModel>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

#include "engine/Color.h"
#include "engine/Lens.h"
#include "ui/CurveEditor.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

namespace rd {

namespace {
const std::vector<std::pair<double, QColor>> kTempGrad{{0.0, QColor("#3d6bd6")}, {0.45, QColor("#c8c8c8")}, {1.0, QColor("#e0b030")}};
const std::vector<std::pair<double, QColor>> kTintGrad{{0.0, QColor("#3fbf4a")}, {0.5, QColor("#c8c8c8")}, {1.0, QColor("#d040c8")}};
const char* kHslNames[8] = {"レッド", "オレンジ", "イエロー", "グリーン", "アクア", "ブルー", "パープル", "マゼンタ"};
const char* kHslColors[8] = {"#e0443e", "#e88a2c", "#e6c229", "#3fae49", "#3cc6c6", "#3b7dd8", "#8a4fd8", "#d040b0"};
const std::pair<const char*, double> kAspects[] = {{"自由", 0}, {"元の比率", -1}, {"1:1", 1.0}, {"4:3", 4.0 / 3}, {"3:2", 1.5},
                                                   {"16:9", 16.0 / 9}, {"5:4", 1.25}, {"4:5（縦）", 0.8}, {"2:3（縦）", 2.0 / 3},
                                                   {"9:16（縦）", 9.0 / 16}};

QLabel* hint(const QString& t) {
    auto* l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#8c8c8c; padding:0 8px; background: transparent;");
    return l;
}
}  // namespace

DevelopPanel::DevelopPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 自動調整
    auto* autoW = new QWidget;
    auto* ah = new QHBoxLayout(autoW);
    ah->setContentsMargins(8, 6, 8, 6);
    auto* autoBtn = new QPushButton(QStringLiteral("自動調整"));
    autoBtn->setObjectName("Accent");
    autoBtn->setToolTip(QStringLiteral("露出・ホワイトバランス・トーンを自動で設定（Ctrl+U）。結果は手で直せます"));
    autoStrength_ = new ScrubField(100, 0, 100, 100, 0, 1, "%", 54);
    connect(autoBtn, &QPushButton::clicked, this, [this] { emit autoRequested(autoStrength_->value(), false); });
    connect(autoStrength_, &ScrubField::valueChanged, this, [this](double v, bool final) {
        if (final) emit autoRequested(static_cast<float>(v), false);
    });
    ah->addWidget(autoBtn, 1);
    ah->addWidget(new QLabel(QStringLiteral("強さ")));
    ah->addWidget(autoStrength_);
    outer->addWidget(autoW);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    v_ = new QVBoxLayout(body);
    v_->setContentsMargins(0, 0, 0, 0);
    v_->setSpacing(2);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    // ---- 基本補正
    auto* basic = new Section(QStringLiteral("基本補正"));
    {
        auto* pw = new QWidget;
        auto* ph = new QHBoxLayout(pw);
        ph->setContentsMargins(8, 2, 8, 2);
        auto* pl = new QLabel(QStringLiteral("プロファイル"));
        pl->setFixedWidth(78);
        pl->setStyleSheet("color:#8c8c8c; background: transparent;");
        profile_ = new QComboBox;
        profile_->addItem(QStringLiteral("カメラに合わせる"), "camera");
        profile_->addItem(QStringLiteral("標準（補正なし）"), "standard");
        profile_->setToolTip(QStringLiteral("カメラに合わせる：RAW に入っているカメラの JPEG と同じくらいの明るさと色を下地にします"));
        connect(profile_, &QComboBox::activated, this, [this](int i) { emit profileChosen(profile_->itemData(i).toString()); });
        ph->addWidget(pl);
        ph->addWidget(profile_, 1);
        basic->add(pw);
    }
    {
        auto* wb = new QWidget;
        auto* h = new QHBoxLayout(wb);
        h->setContentsMargins(8, 2, 8, 2);
        auto* lab = new QLabel("WB");
        lab->setFixedWidth(78);
        lab->setStyleSheet("color:#8c8c8c; background: transparent;");
        wbMode_ = new QComboBox;
        wbMode_->addItems({QStringLiteral("撮影時の設定"), QStringLiteral("自動"), QStringLiteral("カスタム"),
                           QStringLiteral("太陽光（5500K）"), QStringLiteral("曇天（6500K）"), QStringLiteral("日陰（7500K）"),
                           QStringLiteral("白熱電球（2850K）"), QStringLiteral("蛍光灯（3800K）"),
                           QStringLiteral("フラッシュ（5500K）")});
        connect(wbMode_, &QComboBox::activated, this, [this](int i) {
            const int presets[] = {0, 0, 0, 5500, 6500, 7500, 2850, 3800, 5500};
            if (i == 0) {
                p_.wbMode = "as_shot";
                p_.temp = static_cast<float>(asShot_.first);
                p_.tint = static_cast<float>(asShot_.second);
                setParams(p_, asShot_);
                emitChange(true, QStringLiteral("WB：撮影時"));
            } else if (i == 1) {
                emit autoRequested(100, true);
            } else if (i >= 3) {
                setWb(presets[i], 0, QStringLiteral("WB プリセット"));
            }
        });
        wbPick_ = new QToolButton;
        wbPick_->setText(QStringLiteral("◎"));
        wbPick_->setCheckable(true);
        wbPick_->setToolTip(QStringLiteral("スポイト：写真の中の白・グレーの部分をクリック（W）"));
        h->addWidget(lab);
        h->addWidget(wbMode_, 1);
        h->addWidget(wbPick_);
        basic->add(wb);
        auto* t = new ParamRow("temp", QStringLiteral("色温度"), kTempMin, kTempMax, 5500, 0, 10, " K", kTempGrad,
                               tempToSlider, sliderToTemp, 64);
        connect(t, &ParamRow::changed, this, &DevelopPanel::rowChanged);
        basic->add(t);
        rows_["temp"] = t;
        row(basic, "tint", QStringLiteral("色かぶり"), -150, 150, 0, 0, 1, {}, kTintGrad);
        row(basic, "exposure", QStringLiteral("露出"), -5, 5, 0, 2, 0.01, " EV");
        row(basic, "contrast", QStringLiteral("コントラスト"), -100, 100, 0, 0, 1);
        row(basic, "highlights", QStringLiteral("ハイライト"), -100, 100, 0, 0, 1);
        row(basic, "shadows", QStringLiteral("シャドウ"), -100, 100, 0, 0, 1);
        row(basic, "whites", QStringLiteral("白レベル"), -100, 100, 0, 0, 1);
        row(basic, "blacks", QStringLiteral("黒レベル"), -100, 100, 0, 0, 1);
        row(basic, "vibrance", QStringLiteral("自然な彩度"), -100, 100, 0, 0, 1);
        row(basic, "saturation", QStringLiteral("彩度"), -100, 100, 0, 0, 1);
    }
    addSection(basic, Panel::Basic);

    // ---- カラーホイール
    auto* wheels = new Section(QStringLiteral("カラーホイール"), true);
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(4, 2, 4, 2);
        h->setSpacing(2);
        const char* names[4] = {"リフト", "ガンマ", "ゲイン", "オフセット"};
        for (int i = 0; i < 4; ++i) {
            wheels_[i] = new ColorWheel(QString::fromUtf8(names[i]));
            connect(wheels_[i], &ColorWheel::changed, this, [this, i](const std::array<float, 3>& v, bool final) {
                if (updating_) return;
                Wheel* ws[4] = {&p_.lift, &p_.gamma, &p_.gain, &p_.offset};
                *ws[i] = v;
                emitChange(final, QStringLiteral("カラーホイール"));
            });
            h->addWidget(wheels_[i]);
        }
        wheels->add(w);
        wheels->add(hint(QStringLiteral("リフト＝暗部　ガンマ＝中間　ゲイン＝明部　オフセット＝全体")));
    }
    addSection(wheels, Panel::Wheels);

    // ---- トーンカーブ
    auto* curve = new Section(QStringLiteral("トーンカーブ"), true);
    curve_ = new CurveEditor;
    connect(curve_->canvas(), &CurveCanvas::changed, this, [this](const std::array<CurvePoints, 4>& c, bool final) {
        if (updating_) return;
        p_.curve = c;
        emitChange(final, QStringLiteral("トーンカーブ"));
    });
    curve->add(curve_);
    curve->add(hint(QStringLiteral("クリック：点を追加　右クリック：削除　ダブルクリック：リセット")));
    addSection(curve, Panel::Curve);

    // ---- HSL
    auto* hsl = new Section(QStringLiteral("HSL / カラー"), true);
    {
        auto* tabs = new QTabWidget;
        const char* kinds[3] = {"色相", "彩度", "輝度"};
        for (int k = 0; k < 3; ++k) {
            auto* page = new QWidget;
            auto* vv = new QVBoxLayout(page);
            vv->setContentsMargins(0, 2, 0, 2);
            vv->setSpacing(1);
            for (int i = 0; i < 8; ++i) {
                auto* r = new ParamRow(QString("hsl:%1:%2").arg(k).arg(i), QString::fromUtf8(kHslNames[i]), -100, 100, 0, 0, 1,
                                       {}, {{0, QColor("#555")}, {0.5, QColor("#777")}, {1, QColor(kHslColors[i])}});
                connect(r, &ParamRow::changed, this, &DevelopPanel::rowChanged);
                vv->addWidget(r);
                hslRows_[k * 8 + i] = r;
            }
            tabs->addTab(page, QString::fromUtf8(kinds[k]));
        }
        hsl->add(tabs);
    }
    addSection(hsl, Panel::Hsl);

    // ---- 肌色
    auto* skin = new Section(QStringLiteral("肌色"), true);
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(8, 2, 8, 2);
        h->addWidget(new QLabel(QStringLiteral("いい感じに")));
        int lv = 1;
        for (const char* t : {"弱め", "普通", "強め"}) {
            auto* b = new QPushButton(QString::fromUtf8(t));
            connect(b, &QPushButton::clicked, this, [this, lv] { emit skinAutoRequested(lv); });
            h->addWidget(b);
            ++lv;
        }
        skin->add(w);
        auto* w2 = new QWidget;
        auto* h2 = new QHBoxLayout(w2);
        h2->setContentsMargins(8, 2, 8, 2);
        skinPick_ = new QToolButton;
        skinPick_->setText(QStringLiteral("◎ 肌をクリックして指定"));
        skinPick_->setCheckable(true);
        skinPick_->setToolButtonStyle(Qt::ToolButtonTextOnly);
        skinPick_->setToolTip(QStringLiteral("写真の中の肌（頬など）をクリックすると、その色を肌として扱います"));
        skinMask_ = new QCheckBox(QStringLiteral("肌の範囲を表示"));
        connect(skinMask_, &QCheckBox::toggled, this, [this](bool on) {
            p_.skinMaskView = on;
            emit skinMaskToggled(on);
        });
        h2->addWidget(skinPick_);
        h2->addWidget(skinMask_);
        h2->addStretch(1);
        skin->add(w2);
    }
    row(skin, "skin.hue", QStringLiteral("色相"), -100, 100, 0, 0, 1, {},
        {{0, QColor("#c0504a")}, {0.5, QColor("#d9a07a")}, {1, QColor("#d8c060")}});
    row(skin, "skin.sat", QStringLiteral("彩度"), -100, 100, 0, 0, 1);
    row(skin, "skin.lum", QStringLiteral("明るさ"), -100, 100, 0, 0, 1);
    skin->add(hint(QStringLiteral("均一化（赤み・黄ぐすみ・まだらを肌の平均に寄せる）")));
    row(skin, "skin.uni_hue", QStringLiteral("色相"), 0, 100, 0, 0, 1);
    row(skin, "skin.uni_sat", QStringLiteral("彩度"), 0, 100, 0, 0, 1);
    row(skin, "skin.uni_lum", QStringLiteral("明るさ"), 0, 100, 0, 0, 1);
    skin->add(hint(QStringLiteral("なめらかさ（シミ・ムラ・小じわを和らげる）")));
    row(skin, "skin.smooth", QStringLiteral("なめらかさ"), 0, 100, 0, 0, 1);
    row(skin, "skin.texture", QStringLiteral("　質感を残す"), 0, 100, 50, 0, 1);
    row(skin, "skin.range", QStringLiteral("範囲"), 0, 100, 50, 0, 1);
    skin->add(hint(QStringLiteral("色相：−で赤み、＋で黄み。範囲：肌とみなす色の広さ（「肌の範囲を表示」で確認）")));
    addSection(skin, Panel::Skin);

    // ---- 部分補正・スポット修復
    auto* local = new Section(QStringLiteral("部分補正・スポット修復"), true);
    {
        auto* w = new QWidget;
        auto* g = new QGridLayout(w);
        g->setContentsMargins(8, 2, 8, 2);
        g->setSpacing(3);
        struct B {
            const char* text;
            const char* tip;
            int tool;       // 描くツール
            const char* add;  // すぐ作るマスク（肌・空）
        };
        const B bs[] = {{"段階", "ドラッグで段階フィルター（空を暗くするなど）", ToolLinear, nullptr},
                        {"円形", "ドラッグで円形フィルター（顔を明るくするなど）", ToolRadial, nullptr},
                        {"ブラシ", "なぞった所だけ補正（Alt で消す）", ToolBrush, nullptr},
                        {"肌", "肌色の所を自動で選ぶ", ToolNone, "skin"},
                        {"空", "空を自動で選ぶ", ToolNone, "sky"},
                        {"スポット修復", "ゴミ・ほくろ・汚れをクリックで消す（Delete で取り消し）", ToolSpot, nullptr}};
        int col = 0, rowi = 0;
        for (const B& b : bs) {
            auto* t = new QToolButton;
            t->setText(QString::fromUtf8(b.text));
            t->setToolTip(QString::fromUtf8(b.tip));
            t->setToolButtonStyle(Qt::ToolButtonTextOnly);
            t->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            if (b.add) {
                const QString type = QString::fromUtf8(b.add);
                connect(t, &QToolButton::clicked, this, [this, type] {
                    LocalMask m;
                    m.type = type;
                    m.name = type == "skin" ? QStringLiteral("肌") : QStringLiteral("空");
                    p_.masks.push_back(m);
                    p_.enabled.local = true;
                    selMask_ = int(p_.masks.size()) - 1;
                    refreshMaskList();
                    syncSections();
                    emitChange(true, QStringLiteral("マスクを追加：") + m.name);
                    emit maskSelectionChanged(selMask_);
                });
            } else {
                t->setCheckable(true);
                const int tool = b.tool;
                toolButtons_[tool] = t;
                connect(t, &QToolButton::clicked, this, [this, tool](bool on) { setLocalTool(on ? LocalTool(tool) : ToolNone); });
            }
            g->addWidget(t, rowi, col);
            if (++col == 3) {
                col = 0;
                ++rowi;
            }
        }
        local->add(w);
        maskList_ = new QListWidget;
        maskList_->setFixedHeight(96);
        local->add(maskList_);
        connect(maskList_, &QListWidget::currentRowChanged, this, [this](int r) {
            if (updating_) return;
            selMask_ = r;
            updateMaskRows();
            emit maskSelectionChanged(r);
        });
        connect(maskList_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
            if (updating_) return;
            const int r = maskList_->row(it);
            if (r < 0 || r >= int(p_.masks.size())) return;
            p_.masks[r].enabled = it->checkState() == Qt::Checked;
            emitChange(true, p_.masks[r].enabled ? QStringLiteral("マスクをオン") : QStringLiteral("マスクをオフ"));
        });
        auto* ops = new QWidget;
        auto* oh = new QHBoxLayout(ops);
        oh->setContentsMargins(8, 0, 8, 0);
        auto mk = [&](const QString& t, auto fn) {
            auto* b = new QPushButton(t);
            connect(b, &QPushButton::clicked, this, fn);
            oh->addWidget(b);
        };
        mk(QStringLiteral("反転"), [this] {
            if (selMask_ < 0 || selMask_ >= int(p_.masks.size())) return;
            p_.masks[selMask_].invert = !p_.masks[selMask_].invert;
            refreshMaskList();
            emitChange(true, QStringLiteral("マスクを反転"));
        });
        mk(QStringLiteral("複製"), [this] {
            if (selMask_ < 0 || selMask_ >= int(p_.masks.size())) return;
            LocalMask m = p_.masks[selMask_];
            m.name += QStringLiteral("のコピー");
            p_.masks.insert(p_.masks.begin() + selMask_ + 1, m);
            ++selMask_;
            refreshMaskList();
            emitChange(true, QStringLiteral("マスクを複製"));
            emit maskSelectionChanged(selMask_);
        });
        mk(QStringLiteral("削除"), [this] {
            if (selMask_ < 0 || selMask_ >= int(p_.masks.size())) return;
            p_.masks.erase(p_.masks.begin() + selMask_);
            selMask_ = std::min(selMask_, int(p_.masks.size()) - 1);
            refreshMaskList();
            emitChange(true, QStringLiteral("マスクを削除"));
            emit maskSelectionChanged(selMask_);
        });
        maskOverlay_ = new QCheckBox(QStringLiteral("範囲を表示"));
        maskOverlay_->setToolTip(QStringLiteral("選んでいるマスクの範囲を赤く表示（O）"));
        connect(maskOverlay_, &QCheckBox::toggled, this, [this](bool on) { emit maskOverlayToggled(on); });
        oh->addWidget(maskOverlay_);
        local->add(ops);

        maskRows_ = new QWidget;
        auto* mv = new QVBoxLayout(maskRows_);
        mv->setContentsMargins(0, 0, 0, 0);
        mv->setSpacing(1);
        auto lrow = [&](const char* key, const char* label, double lo, double hi, int dec, double step,
                        std::vector<std::pair<double, QColor>> grad = {}) {
            auto* r = new ParamRow(QString::fromUtf8(key), QString::fromUtf8(label), lo, hi, 0, dec, step, {}, std::move(grad));
            connect(r, &ParamRow::changed, this, &DevelopPanel::rowChanged);
            mv->addWidget(r);
            rows_[QString::fromUtf8(key)] = r;
        };
        lrow("local.exposure", "露出", -4, 4, 2, 0.05);
        lrow("local.contrast", "コントラスト", -100, 100, 0, 1);
        lrow("local.highlights", "ハイライト", -100, 100, 0, 1);
        lrow("local.shadows", "シャドウ", -100, 100, 0, 1);
        lrow("local.temp", "色温度", -100, 100, 0, 1, {{0, QColor("#4a78c8")}, {0.5, QColor("#888")}, {1, QColor("#d8a640")}});
        lrow("local.tint", "色かぶり", -100, 100, 0, 1, {{0, QColor("#4caf50")}, {0.5, QColor("#888")}, {1, QColor("#c050c0")}});
        lrow("local.saturation", "彩度", -100, 100, 0, 1);
        lrow("local.feather", "ぼかし", 0, 100, 0, 1);
        rows_["local.feather"]->setValue(50);
        local->add(maskRows_);

        brushRows_ = new QWidget;
        auto* bv = new QVBoxLayout(brushRows_);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(1);
        brushSize_ = new ParamRow("brush.size", QStringLiteral("ブラシの大きさ"), 0.2, 20, 3, 1, 0.1, "%");
        brushFlow_ = new ParamRow("brush.flow", QStringLiteral("流量"), 5, 100, 100, 0, 1, "%");
        brushErase_ = new QCheckBox(QStringLiteral("消しゴム（Alt を押しながらでも）"));
        auto* bw = new QWidget;
        auto* bh = new QHBoxLayout(bw);
        bh->setContentsMargins(8, 0, 8, 0);
        bh->addWidget(brushErase_);
        bv->addWidget(brushSize_);
        bv->addWidget(brushFlow_);
        bv->addWidget(bw);
        spotSize_ = new ParamRow("spot.size", QStringLiteral("スポットの大きさ"), 0.1, 10, 1, 1, 0.1, "%");
        spotClone_ = new QCheckBox(QStringLiteral("コピー（なじませない）"));
        auto* sw = new QWidget;
        auto* sh = new QHBoxLayout(sw);
        sh->setContentsMargins(8, 0, 8, 0);
        sh->addWidget(spotClone_);
        auto* clear = new QPushButton(QStringLiteral("スポットを全部消す"));
        connect(clear, &QPushButton::clicked, this, [this] {
            if (p_.spots.empty()) return;
            p_.spots.clear();
            emitChange(true, QStringLiteral("スポット修復をすべて削除"));
        });
        sh->addWidget(clear);
        bv->addWidget(spotSize_);
        bv->addWidget(sw);
        local->add(brushRows_);
        local->add(hint(QStringLiteral("段階・円形：ドラッグで作成、丸い印をドラッグで調整。スポット：クリックで修復、点線の円（コピー元）は動かせます")));
        updateMaskRows();
    }
    addSection(local, Panel::Local);

    // ---- ディテール
    auto* detail = new Section(QStringLiteral("ディテール"), true);
    row(detail, "sharpen.amount", QStringLiteral("シャープ 量"), 0, 150, 40, 0, 1);
    row(detail, "sharpen.radius", QStringLiteral("半径"), 0.5, 3.0, 1.0, 1, 0.1);
    row(detail, "sharpen.masking", QStringLiteral("マスク"), 0, 100, 0, 0, 1);
    detail->add(hint(QStringLiteral("ノイズ軽減")));
    row(detail, "nr.luma", QStringLiteral("輝度"), 0, 100, 0, 0, 1);
    row(detail, "nr.luma_detail", QStringLiteral("　ディテール"), 0, 100, 50, 0, 1);
    row(detail, "nr.luma_contrast", QStringLiteral("　コントラスト"), 0, 100, 0, 0, 1);
    row(detail, "nr.color", QStringLiteral("カラー"), 0, 100, 25, 0, 1);
    row(detail, "nr.color_detail", QStringLiteral("　ディテール"), 0, 100, 50, 0, 1);
    row(detail, "nr.color_smooth", QStringLiteral("　滑らかさ"), 0, 100, 50, 0, 1);
    detail->add(hint(QStringLiteral("シャープ・ノイズ低減は、ズームしたときの高精細表示で確認してください")));
    addSection(detail, Panel::Detail);

    // ---- 切り抜き
    auto* crop = new Section(QStringLiteral("切り抜きと角度"), true);
    {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(8, 2, 8, 2);
        cropBtn_ = new QPushButton(QStringLiteral("切り抜きツール (R)"));
        cropBtn_->setCheckable(true);
        connect(cropBtn_, &QPushButton::toggled, this, &DevelopPanel::cropToolToggled);
        aspect_ = new QComboBox;
        for (auto& [name, val] : kAspects) aspect_->addItem(QString::fromUtf8(name), val);
        connect(aspect_, &QComboBox::currentIndexChanged, this, &DevelopPanel::aspectChanged);
        h->addWidget(cropBtn_);
        h->addWidget(aspect_);
        crop->add(w);
        row(crop, "crop.angle", QStringLiteral("角度"), -45, 45, 0, 2, 0.05, "°");
    }
    addSection(crop, Panel::Crop);

    // ---- レンズ補正
    auto* lens = new Section(QStringLiteral("レンズ補正"), true);
    {
        auto* tabs = new QTabWidget;
        auto* prof = new QWidget;
        auto* pv = new QVBoxLayout(prof);
        pv->setContentsMargins(8, 4, 8, 4);
        lensProfile_ = new QCheckBox(QStringLiteral("プロファイル補正を使用"));
        lensStatus_ = new QLabel;
        lensStatus_->setWordWrap(true);
        lensStatus_->setStyleSheet("color:#8c8c8c; background: transparent;");
        // メーカー → レンズの 2 段で選ぶ。名前の一部で全メーカーから検索もできる
        auto* grid = new QGridLayout;
        grid->setContentsMargins(0, 0, 0, 0);
        lensMaker_ = new QComboBox;
        lensMaker_->addItem(QStringLiteral("自動（EXIF から検出）"), QString());
        {
            QMap<QString, int> counts;
            QMap<QString, QString> canon;  // 大文字小文字の違いをまとめる
            for (const auto& k : LensDb::get().allKeys()) {
                const QString key = QString::fromStdString(k);
                const QString mk = key.section('|', 0, 0).trimmed();
                const QString low = mk.toLower();
                if (!canon.contains(low)) canon[low] = mk;
                counts[canon[low]]++;
            }
            QStringList makers = counts.keys();
            std::sort(makers.begin(), makers.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
            for (const QString& m : makers) lensMaker_->addItem(QStringLiteral("%1（%2）").arg(m).arg(counts[m]), m);
        }
        lensCombo_ = new QComboBox;
        lensCombo_->setMinimumContentsLength(6);
        lensCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        lensCombo_->setMaxVisibleItems(25);
        lensMaker_->setMaxVisibleItems(25);
        lensSearch_ = new QLineEdit;
        lensSearch_->setPlaceholderText(QStringLiteral("名前で検索（例：24-70）"));
        lensSearch_->setClearButtonEnabled(true);
        QStringList names;
        const auto& db = LensDb::get();
        for (const auto& k : db.allKeys()) names << QString::fromStdString(db.displayName(k));
        auto* comp = new QCompleter(names, lensSearch_);
        comp->setCaseSensitivity(Qt::CaseInsensitive);
        comp->setFilterMode(Qt::MatchContains);
        comp->setMaxVisibleItems(20);
        lensSearch_->setCompleter(comp);
        grid->addWidget(new QLabel(QStringLiteral("メーカー")), 0, 0);
        grid->addWidget(lensMaker_, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("レンズ")), 1, 0);
        grid->addWidget(lensCombo_, 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("検索")), 2, 0);
        grid->addWidget(lensSearch_, 2, 1);
        grid->setColumnStretch(1, 1);
        auto* hl = grid;
        connect(comp, qOverload<const QString&>(&QCompleter::activated), this, [this](const QString& text) {
            const auto& db = LensDb::get();
            for (const auto& k : db.allKeys())
                if (QString::fromStdString(db.displayName(k)) == text) {
                    p_.lensKey = QString::fromStdString(k);
                    p_.enabled.lens = true;
                    updating_ = true;
                    fillLensModels(p_.lensKey.section('|', 0, 0).trimmed(), p_.lensKey);
                    updating_ = false;
                    QTimer::singleShot(0, lensSearch_, &QLineEdit::clear);
                    syncSections();
                    emitChange(true, QStringLiteral("レンズを選択"));
                    return;
                }
        });
        connect(lensMaker_, &QComboBox::activated, this, [this](int i) {
            if (updating_) return;
            const QString mk = lensMaker_->itemData(i).toString();
            if (mk.isEmpty()) {
                fillLensModels({}, {});
                p_.lensKey.clear();
                p_.enabled.lens = true;
                syncSections();
                emitChange(true, QStringLiteral("レンズ：自動"));
            } else {
                fillLensModels(mk, {});
                lensCombo_->showPopup();  // 続けてレンズを選べるように
            }
        });
        pv->addWidget(lensProfile_);
        pv->addWidget(lensStatus_);
        pv->addLayout(hl);
        fillLensModels({}, {});
        for (auto [key, label] : {std::pair{"lens.distortion", "歪曲"}, std::pair{"lens.vignetting", "周辺光量"}}) {
            auto* r = new ParamRow(key, QString::fromUtf8(label), 0, 200, 100, 0, 1, "%");
            connect(r, &ParamRow::changed, this, &DevelopPanel::rowChanged);
            pv->addWidget(r);
            rows_[key] = r;
        }
        lensCa_ = new QCheckBox(QStringLiteral("色収差を除去"));
        lensTrim_ = new QCheckBox(QStringLiteral("端の空白を自動トリミング"));
        pv->addWidget(lensCa_);
        pv->addWidget(lensTrim_);
        tabs->addTab(prof, QStringLiteral("プロファイル"));
        auto* man = new QWidget;
        auto* mv = new QVBoxLayout(man);
        mv->setContentsMargins(0, 4, 0, 4);
        for (auto [key, label] : {std::pair{"lens.manual_distortion", "歪曲"}, std::pair{"lens.manual_vignetting", "周辺光量"}}) {
            auto* r = new ParamRow(key, QString::fromUtf8(label), -100, 100, 0, 0, 1);
            connect(r, &ParamRow::changed, this, &DevelopPanel::rowChanged);
            mv->addWidget(r);
            rows_[key] = r;
        }
        mv->addStretch(1);
        tabs->addTab(man, QStringLiteral("手動"));
        lens->add(tabs);
        auto onCheck = [this](QCheckBox* cb, bool Params::*field, const QString& desc) {
            connect(cb, &QCheckBox::toggled, this, [this, field, desc](bool on) {
                if (updating_) return;
                p_.*field = on;
                p_.enabled.lens = true;
                syncSections();
                emitChange(true, desc);
            });
        };
        onCheck(lensProfile_, &Params::lensProfile, QStringLiteral("レンズプロファイル"));
        onCheck(lensCa_, &Params::lensCa, QStringLiteral("色収差除去"));
        onCheck(lensTrim_, &Params::lensAutotrim, QStringLiteral("自動トリミング"));
        connect(lensCombo_, &QComboBox::activated, this, [this](int i) {
            if (updating_) return;
            if (lensCombo_->itemData(i).toString().isEmpty()) return;  // 「選んでください」
            p_.lensKey = lensCombo_->itemData(i).toString();
            p_.enabled.lens = true;
            syncSections();
            emitChange(true, QStringLiteral("レンズを選択"));
        });
        if (!db.ok()) lensStatus_->setText(QStringLiteral("レンズデータベースが見つかりません（手動補正は使えます）"));
    }
    addSection(lens, Panel::Lens);
    v_->addStretch(1);
}

ParamRow* DevelopPanel::row(Section* sec, const QString& key, const QString& label, double lo, double hi, double def,
                            int dec, double step, const QString& suffix, std::vector<std::pair<double, QColor>> grad) {
    auto* r = new ParamRow(key, label, lo, hi, def, dec, step, suffix, std::move(grad));
    connect(r, &ParamRow::changed, this, &DevelopPanel::rowChanged);
    connect(r, &ParamRow::hovered, this, [this](const QString& k, bool on) { emit hoverKey(on ? k : QString()); });
    sec->add(r);
    rows_[key] = r;
    return r;
}

void DevelopPanel::addSection(Section* sec, Panel panel) {
    sections_[panel] = sec;
    connect(sec, &Section::toggled, this, [this, panel](bool on) {
        enabledFlag(p_, panel) = on;
        soloBackup_.reset();
        emitChange(true, QStringLiteral("%1を%2").arg(QString::fromUtf8(panelName(panel)), on ? QStringLiteral("オン") : QStringLiteral("オフ")));
    });
    connect(sec, &Section::soloRequested, this, [this, panel] { solo(panel); });
    connect(sec, &Section::resetRequested, this, [this, panel] {
        resetPanel(p_, panel);
        if (panel == Panel::Basic) {
            p_.temp = static_cast<float>(asShot_.first);
            p_.tint = static_cast<float>(asShot_.second);
        }
        setParams(p_, asShot_);
        emitChange(true, QStringLiteral("%1をリセット").arg(QString::fromUtf8(panelName(panel))));
    });
    v_->addWidget(sec);
}

void DevelopPanel::solo(Panel panel) {
    // もう一度押すと元に戻る
    if (soloBackup_) {
        p_.enabled = *soloBackup_;
        soloBackup_.reset();
        emitChange(true, QStringLiteral("ソロを解除"));
    } else {
        soloBackup_ = p_.enabled;
        for (Panel q : {Panel::Basic, Panel::Wheels, Panel::Curve, Panel::Hsl, Panel::Skin, Panel::Local, Panel::Detail, Panel::Lens, Panel::Crop})
            enabledFlag(p_, q) = (q == panel);
        emitChange(true, QStringLiteral("%1だけオン").arg(QString::fromUtf8(panelName(panel))));
    }
    syncSections();
}

void DevelopPanel::syncSections() {
    for (auto it = sections_.begin(); it != sections_.end(); ++it) it.value()->setEnabledState(enabledFlag(p_, it.key()));
}

void DevelopPanel::setParams(const Params& p, std::pair<double, double> asShot) {
    updating_ = true;
    p_ = p;
    asShot_ = asShot;
    const bool shot = p_.wbMode == "as_shot";
    rows_["temp"]->setValue(shot ? asShot.first : p_.temp);
    rows_["tint"]->setValue(shot ? asShot.second : p_.tint);
    wbMode_->setCurrentIndex(shot ? 0 : 2);
    profile_->setCurrentIndex(p_.cameraLook() ? 0 : 1);
    rows_["exposure"]->setValue(p_.exposure);
    rows_["contrast"]->setValue(p_.contrast);
    rows_["highlights"]->setValue(p_.highlights);
    rows_["shadows"]->setValue(p_.shadows);
    rows_["whites"]->setValue(p_.whites);
    rows_["blacks"]->setValue(p_.blacks);
    rows_["vibrance"]->setValue(p_.vibrance);
    rows_["saturation"]->setValue(p_.saturation);
    const Wheel ws[4] = {p_.lift, p_.gamma, p_.gain, p_.offset};
    for (int i = 0; i < 4; ++i) wheels_[i]->setValues(ws[i]);
    curve_->canvas()->setCurves(p_.curve);
    for (int i = 0; i < 8; ++i) {
        hslRows_[i]->setValue(p_.hslHue[i]);
        hslRows_[8 + i]->setValue(p_.hslSat[i]);
        hslRows_[16 + i]->setValue(p_.hslLum[i]);
    }
    rows_["sharpen.amount"]->setValue(p_.sharpenAmount);
    rows_["sharpen.radius"]->setValue(p_.sharpenRadius);
    rows_["sharpen.masking"]->setValue(p_.sharpenMasking);
    rows_["nr.luma"]->setValue(p_.nrLuma);
    rows_["nr.color"]->setValue(p_.nrColor);
    rows_["skin.hue"]->setValue(p_.skinHue);
    rows_["skin.sat"]->setValue(p_.skinSat);
    rows_["skin.lum"]->setValue(p_.skinLum);
    rows_["skin.uni_hue"]->setValue(p_.skinUniHue);
    rows_["skin.uni_sat"]->setValue(p_.skinUniSat);
    rows_["skin.uni_lum"]->setValue(p_.skinUniLum);
    rows_["skin.range"]->setValue(p_.skinRange);
    rows_["skin.smooth"]->setValue(p_.skinSmooth);
    rows_["skin.texture"]->setValue(p_.skinTexture);
    p_.skinMaskView = skinMask_->isChecked();
    if (selMask_ >= int(p_.masks.size())) selMask_ = int(p_.masks.size()) - 1;
    refreshMaskList();
    rows_["nr.luma_detail"]->setValue(p_.nrLumaDetail);
    rows_["nr.luma_contrast"]->setValue(p_.nrLumaContrast);
    rows_["nr.color_detail"]->setValue(p_.nrColorDetail);
    rows_["nr.color_smooth"]->setValue(p_.nrColorSmooth);
    rows_["crop.angle"]->setValue(p_.cropAngle);
    rows_["lens.distortion"]->setValue(p_.lensDistortion);
    rows_["lens.vignetting"]->setValue(p_.lensVignetting);
    rows_["lens.manual_distortion"]->setValue(p_.lensManualDistortion);
    rows_["lens.manual_vignetting"]->setValue(p_.lensManualVignetting);
    lensProfile_->setChecked(p_.lensProfile);
    lensCa_->setChecked(p_.lensCa);
    lensTrim_->setChecked(p_.lensAutotrim);
    fillLensModels(p_.lensKey.section('|', 0, 0).trimmed(), p_.lensKey);
    syncSections();
    updating_ = false;
}

void DevelopPanel::setHistogram(const std::vector<float>& h) { curve_->canvas()->setHistogram(h); }

double DevelopPanel::aspectValue() const { return aspect_->currentData().toDouble(); }

void DevelopPanel::fillLensModels(const QString& maker, const QString& selectKey) {
    lensCombo_->clear();
    if (maker.isEmpty()) {
        lensMaker_->setCurrentIndex(0);
        lensCombo_->addItem(QStringLiteral("（自動で検出したレンズ）"), QString());
        lensCombo_->setEnabled(false);
        return;
    }
    for (int i = 1; i < lensMaker_->count(); ++i)
        if (lensMaker_->itemData(i).toString().compare(maker, Qt::CaseInsensitive) == 0) lensMaker_->setCurrentIndex(i);
    lensCombo_->setEnabled(true);
    lensCombo_->addItem(QStringLiteral("レンズを選んでください"), QString());
    // マウントごとに見出しを付ける（複数マウント対応・レンズ一体型カメラは最後に）
    struct It {
        QString group, name, key;
    };
    QList<It> items;
    const auto& db = LensDb::get();
    for (const auto& k : db.allKeys()) {
        const QString key = QString::fromStdString(k);
        if (key.section('|', 0, 0).trimmed().compare(maker, Qt::CaseInsensitive) != 0) continue;
        const std::string m = db.mountOf(k);
        const QString g = m == "fixed" ? QStringLiteral("~2") : m == "*" ? QStringLiteral("~1") : QString::fromStdString(m);
        QString name = key.section('|', 1).trimmed();
        if (name.startsWith(maker + ' ', Qt::CaseInsensitive)) name = name.mid(maker.size() + 1);  // 「Canon」の重複を省く
        items.append({g, name, key});
    }
    // 数字は数として比べる（14mm → 100mm の順に）
    auto natural = [](const QString& a, const QString& b) {
        int i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (a[i].isDigit() && b[j].isDigit()) {
                int i2 = i, j2 = j;
                while (i2 < a.size() && (a[i2].isDigit() || a[i2] == '.')) ++i2;
                while (j2 < b.size() && (b[j2].isDigit() || b[j2] == '.')) ++j2;
                const double x = a.mid(i, i2 - i).toDouble(), y = b.mid(j, j2 - j).toDouble();
                if (x != y) return x < y ? -1 : 1;
                i = i2;
                j = j2;
            } else {
                const QChar ca = a[i].toLower(), cb = b[j].toLower();
                if (ca != cb) return ca < cb ? -1 : 1;
                ++i;
                ++j;
            }
        }
        return int((a.size() - i) - (b.size() - j));
    };
    std::sort(items.begin(), items.end(), [&](const It& a, const It& b) {
        if (a.group != b.group) return natural(a.group, b.group) < 0;
        return natural(a.name, b.name) < 0;
    });
    auto* model = qobject_cast<QStandardItemModel*>(lensCombo_->model());
    QString cur = "\x01";
    for (const auto& it : items) {
        if (it.group != cur) {
            cur = it.group;
            const QString title = cur == "~1" ? QStringLiteral("複数のマウントに対応")
                                : cur == "~2" ? QStringLiteral("レンズ一体型カメラ")
                                              : cur + QStringLiteral(" マウント");
            lensCombo_->addItem(QStringLiteral("── ") + title + QStringLiteral(" ──"), QString());
            if (model) {
                QStandardItem* h = model->item(lensCombo_->count() - 1);
                h->setFlags(Qt::NoItemFlags);  // 選べない見出し
                QFont f = h->font();
                f.setBold(true);
                h->setFont(f);
                h->setForeground(QColor(0xd2, 0x8b, 0x2c));
            }
        }
        lensCombo_->addItem(QStringLiteral("　") + it.name, it.key);
    }
    const int i = lensCombo_->findData(selectKey);
    lensCombo_->setCurrentIndex(i < 0 ? 0 : i);
}

void DevelopPanel::setLocalTool(LocalTool t) {
    localTool_ = t;
    for (auto it = toolButtons_.begin(); it != toolButtons_.end(); ++it) it.value()->setChecked(it.key() == t && t != ToolNone);
    brushRows_->setVisible(true);
    brushSize_->setVisible(t == ToolBrush);
    brushFlow_->setVisible(t == ToolBrush);
    brushErase_->parentWidget()->setVisible(t == ToolBrush);
    spotSize_->setVisible(t == ToolSpot);
    spotClone_->parentWidget()->setVisible(t == ToolSpot);
    emit localToolChanged(t);
}

void DevelopPanel::selectMask(int i) {
    selMask_ = i;
    refreshMaskList();
}

bool DevelopPanel::maskOverlay() const { return maskOverlay_ && maskOverlay_->isChecked(); }
double DevelopPanel::brushSize() const { return brushSize_->value() / 100.0; }
double DevelopPanel::brushFlow() const { return brushFlow_->value() / 100.0; }
bool DevelopPanel::brushErase() const { return brushErase_->isChecked(); }
double DevelopPanel::spotSize() const { return spotSize_->value() / 100.0; }
bool DevelopPanel::spotClone() const { return spotClone_->isChecked(); }
void DevelopPanel::setBrushSize(double r) { brushSize_->setValue(std::clamp(r * 100.0, 0.2, 20.0)); }

void DevelopPanel::refreshMaskList() {
    const bool old = updating_;
    updating_ = true;
    maskList_->clear();
    static const QMap<QString, QString> names{{"linear", QStringLiteral("段階")}, {"radial", QStringLiteral("円形")},
                                              {"brush", QStringLiteral("ブラシ")}, {"skin", QStringLiteral("肌")},
                                              {"sky", QStringLiteral("空")}};
    for (size_t i = 0; i < p_.masks.size(); ++i) {
        const LocalMask& m = p_.masks[i];
        QString t = QStringLiteral("%1. %2").arg(i + 1).arg(m.name.isEmpty() ? names.value(m.type, m.type) : m.name);
        if (m.invert) t += QStringLiteral("（反転）");
        auto* it = new QListWidgetItem(t);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(m.enabled ? Qt::Checked : Qt::Unchecked);
        maskList_->addItem(it);
    }
    if (selMask_ >= 0 && selMask_ < maskList_->count()) maskList_->setCurrentRow(selMask_);
    updating_ = old;
    updateMaskRows();
}

void DevelopPanel::updateMaskRows() {
    const bool has = selMask_ >= 0 && selMask_ < int(p_.masks.size());
    maskRows_->setEnabled(has);
    if (!has) return;
    const LocalMask& m = p_.masks[selMask_];
    const bool old = updating_;
    updating_ = true;
    rows_["local.exposure"]->setValue(m.exposure);
    rows_["local.contrast"]->setValue(m.contrast);
    rows_["local.highlights"]->setValue(m.highlights);
    rows_["local.shadows"]->setValue(m.shadows);
    rows_["local.temp"]->setValue(m.temp);
    rows_["local.tint"]->setValue(m.tint);
    rows_["local.saturation"]->setValue(m.saturation);
    rows_["local.feather"]->setValue(m.feather);
    updating_ = old;
}

bool DevelopPanel::skinMaskShown() const { return skinMask_ && skinMask_->isChecked(); }

void DevelopPanel::setLensStatus(const QString& text) {
    if (LensDb::get().ok()) lensStatus_->setText(text);
}

void DevelopPanel::rowChanged(const QString& key, double v, bool final) {
    if (updating_) return;
    const float f = static_cast<float>(v);
    QString desc = key;
    static const QMap<QString, QString> names{{"temp", "色温度"}, {"tint", "色かぶり"}, {"exposure", "露出"},
                                              {"contrast", "コントラスト"}, {"highlights", "ハイライト"},
                                              {"shadows", "シャドウ"}, {"whites", "白レベル"}, {"blacks", "黒レベル"},
                                              {"vibrance", "自然な彩度"}, {"saturation", "彩度"},
                                              {"sharpen.amount", "シャープ"}, {"sharpen.radius", "シャープ半径"},
                                              {"sharpen.masking", "シャープのマスク"}, {"nr.luma", "輝度ノイズ"},
                                              {"nr.color", "カラーノイズ"}, {"skin.hue", "肌の色相"}, {"skin.sat", "肌の彩度"},
                                              {"skin.lum", "肌の明るさ"}, {"skin.uni_hue", "肌の均一化（色相）"},
                                              {"skin.uni_sat", "肌の均一化（彩度）"}, {"skin.uni_lum", "肌の均一化（明るさ）"},
                                              {"skin.range", "肌の範囲"}, {"local.exposure", "部分補正：露出"}, {"local.contrast", "部分補正：コントラスト"},
                                              {"local.highlights", "部分補正：ハイライト"}, {"local.shadows", "部分補正：シャドウ"},
                                              {"local.temp", "部分補正：色温度"}, {"local.tint", "部分補正：色かぶり"},
                                              {"local.saturation", "部分補正：彩度"}, {"local.feather", "部分補正：ぼかし"}, {"skin.smooth", "肌のなめらかさ"}, {"skin.texture", "肌の質感"}, {"nr.luma_detail", "輝度ノイズのディテール"},
                                              {"nr.luma_contrast", "輝度ノイズのコントラスト"}, {"nr.color_detail", "カラーノイズのディテール"},
                                              {"nr.color_smooth", "カラーノイズの滑らかさ"}, {"crop.angle", "角度"},
                                              {"lens.distortion", "歪曲（プロファイル）"}, {"lens.vignetting", "周辺光量（プロファイル）"},
                                              {"lens.manual_distortion", "歪曲（手動）"}, {"lens.manual_vignetting", "周辺光量（手動）"}};
    if (key == "temp" || key == "tint") {
        if (p_.wbMode == "as_shot") {
            p_.temp = static_cast<float>(asShot_.first);
            p_.tint = static_cast<float>(asShot_.second);
        }
        p_.wbMode = "custom";
        (key == "temp" ? p_.temp : p_.tint) = f;
        wbMode_->setCurrentIndex(2);
    } else if (key.startsWith("hsl:")) {
        const auto parts = key.split(':');
        const int k = parts[1].toInt(), i = parts[2].toInt();
        (k == 0 ? p_.hslHue : k == 1 ? p_.hslSat : p_.hslLum)[i] = f;
        desc = QStringLiteral("HSL");
    } else if (key == "exposure") p_.exposure = f;
    else if (key == "contrast") p_.contrast = f;
    else if (key == "highlights") p_.highlights = f;
    else if (key == "shadows") p_.shadows = f;
    else if (key == "whites") p_.whites = f;
    else if (key == "blacks") p_.blacks = f;
    else if (key == "vibrance") p_.vibrance = f;
    else if (key == "saturation") p_.saturation = f;
    else if (key == "sharpen.amount") p_.sharpenAmount = f;
    else if (key == "sharpen.radius") p_.sharpenRadius = f;
    else if (key == "sharpen.masking") p_.sharpenMasking = f;
    else if (key == "nr.luma") p_.nrLuma = f;
    else if (key == "nr.color") p_.nrColor = f;
    else if (key.startsWith("local.")) {
        if (selMask_ < 0 || selMask_ >= int(p_.masks.size())) return;
        LocalMask& m = p_.masks[selMask_];
        const QString k = key.mid(6);
        if (k == "exposure") m.exposure = f;
        else if (k == "contrast") m.contrast = f;
        else if (k == "highlights") m.highlights = f;
        else if (k == "shadows") m.shadows = f;
        else if (k == "temp") m.temp = f;
        else if (k == "tint") m.tint = f;
        else if (k == "saturation") m.saturation = f;
        else if (k == "feather") m.feather = f;
    }
    else if (key == "skin.hue") p_.skinHue = f;
    else if (key == "skin.sat") p_.skinSat = f;
    else if (key == "skin.lum") p_.skinLum = f;
    else if (key == "skin.uni_hue") p_.skinUniHue = f;
    else if (key == "skin.uni_sat") p_.skinUniSat = f;
    else if (key == "skin.uni_lum") p_.skinUniLum = f;
    else if (key == "skin.range") p_.skinRange = f;
    else if (key == "skin.smooth") p_.skinSmooth = f;
    else if (key == "skin.texture") p_.skinTexture = f;
    else if (key == "nr.luma_detail") p_.nrLumaDetail = f;
    else if (key == "nr.luma_contrast") p_.nrLumaContrast = f;
    else if (key == "nr.color_detail") p_.nrColorDetail = f;
    else if (key == "nr.color_smooth") p_.nrColorSmooth = f;
    else if (key == "crop.angle") p_.cropAngle = f;
    else if (key == "lens.distortion") p_.lensDistortion = f;
    else if (key == "lens.vignetting") p_.lensVignetting = f;
    else if (key == "lens.manual_distortion") p_.lensManualDistortion = f;
    else if (key == "lens.manual_vignetting") p_.lensManualVignetting = f;
    // そのパネルがオフのまま値を動かしたら、効果が見えるようにオンにする
    const Panel panel = key.startsWith("hsl:") ? Panel::Hsl
                        : key.startsWith("skin.") ? Panel::Skin
                        : key.startsWith("local.") ? Panel::Local
                        : key.startsWith("sharpen") || key.startsWith("nr") ? Panel::Detail
                        : key == "crop.angle" ? Panel::Crop
                        : key.startsWith("lens.") ? Panel::Lens
                                                  : Panel::Basic;
    if (!enabledFlag(p_, panel)) {
        enabledFlag(p_, panel) = true;
        syncSections();
    }
    emitChange(final, names.value(key, desc));
}

void DevelopPanel::emitChange(bool final, const QString& desc) { emit paramsChanged(p_, final, desc); }

void DevelopPanel::setWb(double temp, double tint, const QString& desc) {
    p_.wbMode = "custom";
    p_.temp = static_cast<float>(temp);
    p_.tint = static_cast<float>(tint);
    p_.enabled.basic = true;
    setParams(p_, asShot_);
    emitChange(true, desc);
}

void DevelopPanel::nudgeZone(const QString& key, double dv, bool final) {
    if (key == "exposure") {
        p_.exposure = static_cast<float>(std::clamp(p_.exposure + dv * 4.0, -5.0, 5.0));
        rows_["exposure"]->setValue(p_.exposure);
        p_.exposure = static_cast<float>(rows_["exposure"]->value());
    } else {
        float* t = key == "blacks" ? &p_.blacks : key == "shadows" ? &p_.shadows : key == "highlights" ? &p_.highlights : &p_.whites;
        *t = static_cast<float>(std::clamp(*t + dv * 200.0, -100.0, 100.0));
        rows_[key]->setValue(*t);
        *t = static_cast<float>(rows_[key]->value());
    }
    p_.enabled.basic = true;
    syncSections();
    emitChange(final, QStringLiteral("ヒストグラム操作"));
}

}  // namespace rd
