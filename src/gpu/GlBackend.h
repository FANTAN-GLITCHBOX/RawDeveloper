#pragma once
// OpenGL 4.3 のコンピュートシェーダーで現像の中心部分を処理する（Windows の AMD・NVIDIA・Intel で動く）
// GL の文脈は専用のスレッドだけで使い、ほかのスレッドからは仕事を頼んで待つ
#include <QString>
#include <memory>

#include "engine/Gpu.h"

class QOffscreenSurface;

namespace rd {

class GlBackend : public GpuBackend {
public:
    // GUI スレッドで呼ぶ（画面外の描画先を作る）。allowSoftware：ソフトウェア描画（llvmpipe など）でも使う（試験用）
    static GlBackend* create(bool allowSoftware, QString* why);
    ~GlBackend() override;
    bool runMain(const MainStage& st, ImageF& out) override;
    QString renderer() const { return renderer_; }

private:
    GlBackend() = default;
    struct Impl;
    std::unique_ptr<Impl> d_;
    QString renderer_;
};

}  // namespace rd
