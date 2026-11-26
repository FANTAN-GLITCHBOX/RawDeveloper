#include "ui/Dialogs.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>

namespace rd {

namespace {
QJsonObject builtin(const QString& name) {
    if (name == QStringLiteral("SNS 用（長辺 2048px・JPEG）"))
        return {{"fmt", "jpeg"}, {"jpeg_level", "normal"}, {"quality", 85}, {"long_edge", 2048}, {"sharpen", "screen"}, {"sharpen_level", 2}};
    if (name == QStringLiteral("印刷用（TIFF 16bit・元サイズ）")) return {{"fmt", "tiff"}, {"quality", 100}, {"long_edge", 0}};
    if (name == QStringLiteral("納品用（JPEG 最高画質・元サイズ）")) return {{"fmt", "jpeg"}, {"jpeg_level", "max"}, {"quality", 98}, {"long_edge", 0}};
    return {};
}
const QStringList kBuiltins{QStringLiteral("SNS 用（長辺 2048px・JPEG）"), QStringLiteral("印刷用（TIFF 16bit・元サイズ）"),
                            QStringLiteral("納品用（JPEG 最高画質・元サイズ）")};
}  // namespace

ExportDialog::ExportDialog(int nSelected, int nFiltered, QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("書き出し"));
    setMinimumWidth(540);
    QSettings st;
    auto* v = new QVBoxLayout(this);

    auto* pr = new QHBoxLayout;
    pr->addWidget(new QLabel(QStringLiteral("設定")));
    preset_ = new QComboBox;
    preset_->addItem(QStringLiteral("（前回の設定）"));
    userPresets_ = QJsonDocument::fromJson(st.value("export/presets", "{}").toByteArray()).object();
    preset_->addItems(kBuiltins);
    preset_->addItems(userPresets_.keys());
    pr->addWidget(preset_, 1);
    auto* save = new QPushButton(QStringLiteral("この設定を保存"));
    pr->addWidget(save);
    v->addLayout(pr);

    auto* g1 = new QGroupBox(QStringLiteral("対象"));
    auto* t = new QVBoxLayout(g1);
    target_ = new QButtonGroup(this);
    auto* tSel = new QRadioButton(QStringLiteral("選択中の写真（%1 枚）").arg(nSelected));
    auto* tAll = new QRadioButton(QStringLiteral("表示中の写真すべて（%1 枚）").arg(nFiltered));
    auto* tStar = new QRadioButton(QStringLiteral("表示中のうち ★ が次以上："));
    auto* tPick = new QRadioButton(QStringLiteral("表示中のうち 採用フラグだけ"));
    target_->addButton(tSel, 0);
    target_->addButton(tAll, 1);
    target_->addButton(tStar, 2);
    target_->addButton(tPick, 3);
    minStar_ = new QSpinBox;
    minStar_->setRange(1, 5);
    minStar_->setValue(st.value("export/min_star", 4).toInt());
    auto* hs = new QHBoxLayout;
    hs->addWidget(tStar);
    hs->addWidget(minStar_);
    hs->addStretch(1);
    t->addWidget(tSel);
    t->addWidget(tAll);
    t->addLayout(hs);
    t->addWidget(tPick);
    tSel->setChecked(true);
    v->addWidget(g1);

    auto* g2 = new QGroupBox(QStringLiteral("形式とサイズ"));
    auto* f2 = new QFormLayout(g2);
    fmt_ = new QComboBox;
    fmt_->addItem("JPEG", "jpeg");
    fmt_->addItem(QStringLiteral("TIFF（16bit）"), "tiff");
    fmt_->addItem(QStringLiteral("PNG（8bit）"), "png");
    jpegLevel_ = new QComboBox;
    jpegLevel_->addItem(QStringLiteral("普通（SNS・Web）"), "normal");
    jpegLevel_->addItem(QStringLiteral("高画質（保存・人に渡す）"), "high");
    jpegLevel_->addItem(QStringLiteral("最高画質（印刷・再編集）"), "max");
    jpegLevel_->addItem(QStringLiteral("カスタム"), "custom");
    jpegLevel_->setToolTip(QStringLiteral("普通：品質 85・色の間引きあり（4:2:0）\n高画質：品質 92・色を間引かない（4:4:4）\n"
                                          "最高画質：品質 98・色を間引かない（4:4:4）\nカスタム：数値を指定（90 以上は 4:4:4）"));
    quality_ = new QSpinBox;
    quality_->setRange(50, 100);
    quality_->setToolTip(QStringLiteral("数値を変えるとカスタムになります"));
    estimate_ = new QLabel;
    estimate_->setStyleSheet("color: gray;");
    auto* qrow = new QHBoxLayout;
    qrow->addWidget(jpegLevel_, 1);
    qrow->addWidget(new QLabel(QStringLiteral("品質")));
    qrow->addWidget(quality_);
    longEdge_ = new QSpinBox;
    longEdge_->setRange(0, 20000);
    longEdge_->setSingleStep(256);
    longEdge_->setSpecialValueText(QStringLiteral("元のサイズ"));
    auto* space = new QComboBox;
    space->addItem(QStringLiteral("sRGB（ICC プロファイル埋め込み）"));
    f2->addRow(QStringLiteral("形式"), fmt_);
    f2->addRow(QStringLiteral("画質（JPEG）"), qrow);
    f2->addRow(QString(), estimate_);
    f2->addRow(QStringLiteral("長辺のピクセル数"), longEdge_);
    f2->addRow(QStringLiteral("色空間"), space);
    auto* sh = new QHBoxLayout;
    sharpen_ = new QComboBox;
    sharpen_->addItem(QStringLiteral("なし"), "none");
    sharpen_->addItem(QStringLiteral("画面用（SNS・Web）"), "screen");
    sharpen_->addItem(QStringLiteral("印刷用（光沢紙）"), "glossy");
    sharpen_->addItem(QStringLiteral("印刷用（マット紙）"), "matte");
    sharpenLevel_ = new QComboBox;
    sharpenLevel_->addItem(QStringLiteral("弱"), 1);
    sharpenLevel_->addItem(QStringLiteral("標準"), 2);
    sharpenLevel_->addItem(QStringLiteral("強"), 3);
    sh->addWidget(sharpen_, 1);
    sh->addWidget(sharpenLevel_);
    f2->addRow(QStringLiteral("書き出し時のシャープ"), sh);
    connect(sharpen_, &QComboBox::currentIndexChanged, this, [this] { sharpenLevel_->setEnabled(sharpen_->currentData() != "none"); });
    v->addWidget(g2);

    auto* g3 = new QGroupBox(QStringLiteral("保存先とファイル名"));
    auto* f3 = new QFormLayout(g3);
    same_ = new QCheckBox(QStringLiteral("元の写真と同じフォルダ"));
    auto* fl = new QHBoxLayout;
    folder_ = new QLineEdit;
    auto* browse = new QPushButton(QStringLiteral("参照…"));
    fl->addWidget(folder_, 1);
    fl->addWidget(browse);
    subfolder_ = new QLineEdit;
    subfolder_->setPlaceholderText(QStringLiteral("例：export（空なら作らない）"));
    template_ = new QLineEdit;
    template_->setToolTip(QStringLiteral("{name}＝元のファイル名、{n}＝連番（0001〜）"));
    suffix_ = new QLineEdit;
    suffix_->setPlaceholderText(QStringLiteral("例：_edit"));
    overwrite_ = new QComboBox;
    overwrite_->addItem(QStringLiteral("同名があれば番号を付ける"), "rename");
    overwrite_->addItem(QStringLiteral("上書きする"), "overwrite");
    overwrite_->addItem(QStringLiteral("書き出さない"), "skip");
    f3->addRow(QString(), same_);
    f3->addRow(QStringLiteral("フォルダ"), fl);
    f3->addRow(QStringLiteral("サブフォルダ"), subfolder_);
    f3->addRow(QStringLiteral("ファイル名"), template_);
    f3->addRow(QStringLiteral("末尾に付ける文字"), suffix_);
    f3->addRow(QStringLiteral("同名ファイル"), overwrite_);
    v->addWidget(g3);

    auto* g4 = new QGroupBox(QStringLiteral("レーティング・ラベル"));
    auto* v4 = new QVBoxLayout(g4);
    embed_ = new QCheckBox(QStringLiteral("★とラベルをファイルに埋め込む（エクスプローラーでも★が見えます）"));
    addLib_ = new QCheckBox(QStringLiteral("書き出したファイルを元の写真と紐付ける（★の変更を反映できる）"));
    v4->addWidget(embed_);
    v4->addWidget(addLib_);
    v->addWidget(g4);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す"));
    bb->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
    connect(bb, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &ExportDialog::reject);
    v->addWidget(bb);

    connect(jpegLevel_, &QComboBox::currentIndexChanged, this, [this] {
        const QString lv = jpegLevel_->currentData().toString();
        if (lv != "custom") {
            settingLevel_ = true;
            quality_->setValue(jpegSetting(lv, quality_->value()).quality);
            settingLevel_ = false;
        }
        updateEstimate();
    });
    connect(quality_, &QSpinBox::valueChanged, this, [this] {
        if (!settingLevel_ && jpegLevel_->currentData() != "custom") {
            settingLevel_ = true;
            jpegLevel_->setCurrentIndex(jpegLevel_->findData("custom"));
            settingLevel_ = false;
        }
        updateEstimate();
    });
    connect(fmt_, &QComboBox::currentIndexChanged, this, [this] {
        const bool jpg = fmt_->currentData() == "jpeg";
        jpegLevel_->setEnabled(jpg);
        quality_->setEnabled(jpg);
        updateEstimate();
    });
    connect(longEdge_, &QSpinBox::valueChanged, this, [this] { updateEstimate(); });
    connect(same_, &QCheckBox::toggled, this, [this](bool on) { folder_->setEnabled(!on); });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("書き出し先フォルダ"), folder_->text());
        if (!d.isEmpty()) folder_->setText(QDir::toNativeSeparators(d));
    });
    connect(preset_, &QComboBox::activated, this, [this](int i) {
        const QString name = preset_->itemText(i);
        QJsonObject d = builtin(name);
        if (d.isEmpty()) d = userPresets_.value(name).toObject();
        if (d.isEmpty()) return;
        QJsonObject cur = options().toJson();
        for (auto it = d.begin(); it != d.end(); ++it) cur[it.key()] = it.value();
        load(ExportOptions::fromJson(cur));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString name = QInputDialog::getText(this, QStringLiteral("書き出し設定の保存"), QStringLiteral("名前")).trimmed();
        if (name.isEmpty()) return;
        QJsonObject d = options().toJson();
        d.remove("folder");
        d.remove("same_folder");
        userPresets_[name] = d;
        QSettings().setValue("export/presets", QJsonDocument(userPresets_).toJson(QJsonDocument::Compact));
        preset_->addItem(name);
    });
    load(ExportOptions::fromJson(QJsonDocument::fromJson(st.value("export/last", "{}").toByteArray()).object()));
}

void ExportDialog::load(const ExportOptions& o) {
    fmt_->setCurrentIndex(std::max(0, fmt_->findData(o.fmt)));
    settingLevel_ = true;
    jpegLevel_->setCurrentIndex(std::max(0, jpegLevel_->findData(o.jpegLevel)));
    quality_->setValue(jpegSetting(o).quality);
    settingLevel_ = false;
    jpegLevel_->setEnabled(o.fmt == "jpeg");
    quality_->setEnabled(o.fmt == "jpeg");
    longEdge_->setValue(o.longEdge);
    same_->setChecked(o.sameFolder);
    folder_->setText(QDir::toNativeSeparators(
        o.folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/RawDeveloper" : o.folder));
    subfolder_->setText(o.subfolder);
    template_->setText(o.nameTemplate);
    suffix_->setText(o.suffix);
    overwrite_->setCurrentIndex(std::max(0, overwrite_->findData(o.overwrite)));
    embed_->setChecked(o.embedRating);
    sharpen_->setCurrentIndex(std::max(0, sharpen_->findData(o.sharpen)));
    sharpenLevel_->setCurrentIndex(std::max(0, sharpenLevel_->findData(o.sharpenLevel)));
    sharpenLevel_->setEnabled(o.sharpen != "none");
    addLib_->setChecked(o.addToLibrary);
    updateEstimate();
}

void ExportDialog::setSample(const QImage& img, QSize fullOut) {
    if (img.isNull() || fullOut.isEmpty()) return;
    QImage s = img.convertToFormat(QImage::Format_RGB888);
    if (std::max(s.width(), s.height()) > 1200) s = s.scaled(1200, 1200, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    sampleW_ = s.width();
    sampleH_ = s.height();
    sample_.resize(static_cast<size_t>(sampleW_) * sampleH_ * 3);
    for (int y = 0; y < sampleH_; ++y)
        memcpy(&sample_[static_cast<size_t>(y) * sampleW_ * 3], s.constScanLine(y), static_cast<size_t>(sampleW_) * 3);
    fullOut_ = fullOut;
    updateEstimate();
}

void ExportDialog::updateEstimate() {
    if (!estimate_) return;
    if (fmt_->currentData() != "jpeg") {
        estimate_->clear();
        return;
    }
    const JpegSetting js = jpegSetting(jpegLevel_->currentData().toString(), quality_->value());
    QString text = QStringLiteral("品質 %1・%2").arg(js.quality).arg(js.chroma444 ? QStringLiteral("色の間引きなし（4:4:4）")
                                                                                  : QStringLiteral("色の間引きあり（4:2:0）"));
    if (!sample_.empty()) {
        int ow = fullOut_.width(), oh = fullOut_.height();
        const int le = longEdge_->value();
        if (le > 0 && std::max(ow, oh) > le) {
            const double k = double(le) / std::max(ow, oh);
            ow = std::max(1, int(std::lround(ow * k)));
            oh = std::max(1, int(std::lround(oh * k)));
        }
        const qint64 b = estimateJpegBytes(sample_.data(), sampleW_, sampleH_, ow, oh, js);
        if (b > 0) {
            const QString sz = b >= 1024 * 1024 ? QString::number(b / 1048576.0, 'f', 1) + " MB" : QString::number(b / 1024) + " KB";
            text += QStringLiteral("\n1 枚あたり約 %1（%2×%3、表示中の写真で計算）").arg(sz).arg(ow).arg(oh);
        }
    }
    estimate_->setText(text);
}

ExportOptions ExportDialog::options() const {
    ExportOptions o;
    o.folder = QDir::fromNativeSeparators(folder_->text());
    o.sameFolder = same_->isChecked();
    o.subfolder = subfolder_->text().trimmed();
    o.fmt = fmt_->currentData().toString();
    o.quality = quality_->value();
    o.jpegLevel = jpegLevel_->currentData().toString();
    o.longEdge = longEdge_->value();
    o.nameTemplate = template_->text().isEmpty() ? "{name}" : template_->text();
    o.suffix = suffix_->text();
    o.embedRating = embed_->isChecked();
    o.overwrite = overwrite_->currentData().toString();
    o.addToLibrary = addLib_->isChecked();
    o.sharpen = sharpen_->currentData().toString();
    o.sharpenLevel = sharpenLevel_->currentData().toInt();
    return o;
}

QString ExportDialog::targetMode() const { return QStringList{"selected", "all", "star", "pick"}[target_->checkedId()]; }
int ExportDialog::minStar() const { return minStar_->value(); }

void ExportDialog::accept() {
    QSettings st;
    st.setValue("export/last", QJsonDocument(options().toJson()).toJson(QJsonDocument::Compact));
    st.setValue("export/min_star", minStar_->value());
    QDialog::accept();
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("環境設定"));
    QSettings st;
    auto* f = new QFormLayout(this);
    autoAdv_ = new QCheckBox(QStringLiteral("★・ラベル・フラグを付けたら次の写真へ進む"));
    autoAdv_->setChecked(st.value("rate/auto_advance", false).toBool());
    writeFiles_ = new QCheckBox(QStringLiteral("JPEG・TIFF・PNG の★・ラベルはファイル自体にも書き込む"));
    writeFiles_->setChecked(st.value("rate/write_files", true).toBool());
    xmpRaw_ = new QCheckBox(QStringLiteral("RAW の★・ラベルを XMP サイドカー（.xmp）にも書く（Lightroom などと共有）"));
    xmpRaw_->setChecked(st.value("rate/xmp_sidecar", false).toBool());
    propagate_ = new QComboBox;
    propagate_->addItem(QStringLiteral("常に反映する"), "always");
    propagate_->addItem(QStringLiteral("その都度確認する"), "ask");
    propagate_->addItem(QStringLiteral("反映しない"), "never");
    propagate_->setCurrentIndex(std::max(0, propagate_->findData(st.value("rate/propagate", "ask"))));
    camProfile_ = new QCheckBox(QStringLiteral("新しく開く RAW は「カメラに合わせる」（カメラの JPEG と同じくらいの明るさと色）から始める"));
    camProfile_->setChecked(st.value("develop/camera_profile", true).toBool());
    gpu_ = new QCheckBox(QStringLiteral("GPU で処理する（次に起動したときから。速くなります）"));
    gpu_->setChecked(st.value("develop/gpu", true).toBool());
    wheel_ = new QCheckBox(QStringLiteral("マウスホイールでスライダー・数値を変更する"));
    wheel_->setChecked(st.value("ui/wheel_adjust", true).toBool());
    f->addRow(QString(), autoAdv_);
    f->addRow(QString(), writeFiles_);
    f->addRow(QString(), xmpRaw_);
    f->addRow(QStringLiteral("元の写真の★を変えたとき\n書き出したファイルにも"), propagate_);
    f->addRow(QString(), camProfile_);
    f->addRow(QString(), gpu_);
    f->addRow(QString(), wheel_);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    f->addRow(bb);
}

void SettingsDialog::accept() {
    QSettings st;
    st.setValue("rate/auto_advance", autoAdv_->isChecked());
    st.setValue("rate/write_files", writeFiles_->isChecked());
    st.setValue("rate/xmp_sidecar", xmpRaw_->isChecked());
    st.setValue("rate/propagate", propagate_->currentData());
    st.setValue("ui/wheel_adjust", wheel_->isChecked());
    st.setValue("develop/camera_profile", camProfile_->isChecked());
    st.setValue("develop/gpu", gpu_->isChecked());
    QDialog::accept();
}

}  // namespace rd
