#include "gpu/GlBackend.h"

#include <QMetaObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QSurfaceFormat>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <mutex>

#include "engine/Color.h"
#include "engine/Pipeline.h"

namespace rd {

namespace {

// ---------------------------------------------------------------- シェーダー
// CPU 版（Pipeline.cpp・Skin.cpp・Local.cpp）と同じ計算をそのまま書き写している。変えるときは両方を直すこと

const char* kCommon = R"(
#version 430
layout(local_size_x = 16, local_size_y = 16) in;
const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
const float kPi = 3.14159265358979;
float ramp(float x, float a, float b) { return clamp((x - a) / (b - a), 0.0, 1.0); }
float smooth01(float t) { t = clamp(t, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
float hueDiff(float a, float b) { return mod(a - b + 540.0, 360.0) - 180.0; }
vec3 rgbToHsv(vec3 c) {
    float r = c.r, g = c.g, b = c.b;
    float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
    float v = mx, s = mx > 0.0 ? d / mx : 0.0, h = 0.0;
    if (d <= 0.0) return vec3(0.0, s, v);
    if (mx == r) h = 60.0 * (g - b) / d;
    else if (mx == g) h = 60.0 * (b - r) / d + 120.0;
    else h = 60.0 * (r - g) / d + 240.0;
    if (h < 0.0) h += 360.0;
    return vec3(h, s, v);
}
vec3 hsvToRgb(float h, float s, float v) {
    if (s <= 0.0) return vec3(v);
    float hh = h / 60.0;
    hh -= 6.0 * floor(hh / 6.0);
    int i = int(hh) % 6;
    float f = hh - floor(hh);
    float p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}
// 肌色
uniform float skCH, skCS, skRange, skHue, skSat, skLum, skUH, skUS;
float skinMask(float h, float s, float v) {
    float f = clamp(skRange / 50.0, 0.2, 2.0);
    float width = 12.0 * f;
    float d = abs(hueDiff(h, skCH));
    float mh = ramp(width - d, 0.0, width * 0.5);
    if (mh <= 0.0) return 0.0;
    float sc = clamp(skCS, 0.08, 0.85);
    float r = s / sc;
    float lo = 1.0 - 0.32 * f, hi = 1.0 + 0.45 * f;
    float ms = ramp(r, lo - 0.2, lo) * (1.0 - ramp(r, hi, hi + 0.4));
    float mv = ramp(v, 0.15, 0.28) * (1.0 - ramp(v, 0.985, 1.0));
    return mh * ms * mv;
}
)";

const char* kMain = R"(
layout(binding = 0) uniform sampler2D inLin;
layout(rgba32f, binding = 1) uniform writeonly image2D outImg;
layout(std430, binding = 2) readonly buffer Tab { float tab[]; };
layout(std430, binding = 3) readonly buffer Hsl { float hslT[]; };
layout(std430, binding = 4) readonly buffer SkinT { float skinTab[]; };
layout(std430, binding = 5) readonly buffer Masks { float mk[]; };
layout(std430, binding = 6) writeonly buffer Show { float showv[]; };
layout(binding = 7) uniform sampler2DArray brushTex;
uniform ivec2 size;
uniform vec3 gains;
uniform int camMat;
uniform float CM[9];
uniform int tabN;
uniform float sScale, satAmt, vibAmt;
uniform int hslOn, hasHue, hasSat, hasLum, skinOn;
uniform int nMask, showSlot;
uniform vec4 mapv;  // nx0, ny0, dx, dy
uniform float asp;  // fullH / fullW
const int MS = 20;

float maskValue(int i, ivec2 px, vec3 disp) {
    int b = i * MS;
    int type = int(mk[b]);
    float v = 0.0;
    float nx = mapv.x + (float(px.x) + 0.5) * mapv.z, ny = mapv.y + (float(px.y) + 0.5) * mapv.w;
    float x0 = mk[b + 4], y0 = mk[b + 5], x1 = mk[b + 6], y1 = mk[b + 7], ang = mk[b + 8], feather = mk[b + 9];
    if (type == 0) {
        float ax = x0, ay = y0 * asp, bx = x1, by = y1 * asp;
        float vx = bx - ax, vy = by - ay, L2 = vx * vx + vy * vy;
        float t = L2 > 1e-12 ? ((nx - ax) * vx + (ny * asp - ay) * vy) / L2 : 0.0;
        v = 1.0 - smooth01(t);
    } else if (type == 1) {
        float a = ang * kPi / 180.0;
        float dx = nx - x0, dy = (ny - y0) * asp;
        float u = cos(a) * dx + sin(a) * dy, w = -sin(a) * dx + cos(a) * dy;
        float rx = max(1e-4, x1), ry = max(1e-4, y1 * asp);
        float e = sqrt((u / rx) * (u / rx) + (w / ry) * (w / ry));
        float f0 = 1.0 - clamp(feather, 1.0, 100.0) / 100.0 * 0.95;
        v = 1.0 - smooth01((e - f0) / (1.0 - f0));
    } else if (type == 2) {
        v = texelFetch(brushTex, ivec3(px, int(mk[b + 3])), 0).r;
    } else {
        vec3 hsv = rgbToHsv(clamp(disp, 0.0, 1.0));
        float hh = hsv.x, ss = hsv.y, vv = hsv.z;
        if (type == 3) {
            v = skinMask(hh, ss, vv);
        } else {
            float blue = smooth01((hh - 175.0) / 20.0) * (1.0 - smooth01((hh - 250.0) / 20.0)) * smooth01((ss - 0.08) / 0.12) *
                         smooth01((vv - 0.35) / 0.2);
            float white = smooth01((vv - 0.78) / 0.12) * (1.0 - smooth01((ss - 0.08) / 0.1));
            float top = 1.0 - smooth01((ny - 0.3) / 0.4);
            v = max(blue * (0.5 + 0.5 * top), white * top * 0.85);
        }
    }
    if (mk[b + 1] > 0.5) v = 1.0 - v;
    return clamp(v, 0.0, 1.0);
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    vec3 xin = texelFetch(inLin, px, 0).rgb * gains;
    if (camMat != 0) {
        vec3 a = xin;
        xin = vec3(CM[0] * a.r + CM[1] * a.g + CM[2] * a.b, CM[3] * a.r + CM[4] * a.g + CM[5] * a.b,
                   CM[6] * a.r + CM[7] * a.g + CM[8] * a.b);
    }
    float mv[16];
    if (nMask > 0) {
        vec3 disp = sqrt(max(xin, vec3(0.0)));
        vec3 lg = vec3(1.0);
        for (int i = 0; i < nMask; ++i) {
            mv[i] = maskValue(i, px, disp);
            int b = i * MS;
            if (mk[b + 2] < 0.5 || mv[i] <= 0.0) continue;
            float e = exp2(mk[b + 10] * mv[i]);
            float t = mk[b + 14] / 100.0 * mv[i], g = mk[b + 15] / 100.0 * mv[i];
            lg *= vec3(e * exp(0.35 * t) * exp(0.12 * g), e * exp(-0.25 * g), e * exp(-0.35 * t) * exp(0.12 * g));
        }
        xin *= lg;
        if (showSlot >= 0) showv[px.y * size.x + px.x] = mv[showSlot];
    }
    vec3 v;
    for (int c = 0; c < 3; ++c) {
        float x = max(xin[c], 0.0);
        int k = min(int(sqrt(x) * sScale + 0.5), tabN - 1);
        v[c] = tab[c * tabN + k];
    }
    if (satAmt != 0.0 || vibAmt != 0.0) {
        float Y = dot(v, kLuma);
        float f = 1.0 + satAmt / 100.0;
        if (vibAmt != 0.0) {
            vec3 hsv = rgbToHsv(clamp(v, 0.0, 1.0));
            float skin = skinTab[int(hsv.x)] * min(hsv.y * 3.0, 1.0);
            float vf = vibAmt / 100.0 * (1.0 - hsv.y) * (1.0 - 0.6 * skin) * (vibAmt > 0.0 ? 1.2 : 1.0);
            f += max(vf, -1.0);
        }
        v = Y + (v - Y) * f;
    }
    if (hslOn != 0) {
        vec3 hsv = rgbToHsv(clamp(v, 0.0, 1.0));
        float hh = hsv.x, ss = hsv.y, vv = hsv.z;
        int bi = clamp(int(hh * 2.0), 0, 720);
        float sw = min(ss * 2.0, 1.0);
        if (hasHue != 0) hh = mod(hh + hslT[bi] * 0.3 * sw + 360.0, 360.0);
        if (hasSat != 0) ss = clamp(ss * (1.0 + hslT[721 + bi] / 100.0), 0.0, 1.0);
        if (hasLum != 0) vv = clamp(vv * (1.0 + hslT[1442 + bi] * 0.006 * sw), 0.0, 1.0);
        v = hsvToRgb(hh, ss, vv);
    }
    if (nMask > 0) {
        for (int i = 0; i < nMask; ++i) {
            int b = i * MS;
            if (mk[b + 2] < 0.5 || mv[i] <= 0.0) continue;
            float a = mv[i];
            float Y = dot(v, kLuma);
            float y0 = clamp(Y, 0.0, 1.0);
            float y = y0;
            float con = mk[b + 11], hl = mk[b + 12], sh = mk[b + 13], sa = mk[b + 16];
            if (con != 0.0) y -= con / 100.0 * a * 0.30 * sin(2.0 * kPi * y) / (2.0 * kPi) * 2.0;
            if (hl != 0.0) y += hl / 100.0 * a * 0.35 * y0 * y0 * (1.0 - y0) * 4.0 * (y0 > 0.5 ? 1.0 : y0 * 2.0);
            if (sh != 0.0) y += sh / 100.0 * a * 0.35 * (1.0 - y0) * (1.0 - y0) * y0 * 4.0 * (y0 < 0.5 ? 1.0 : (1.0 - y0) * 2.0);
            v += y - y0;
            if (sa != 0.0) {
                Y = dot(v, kLuma);
                v = Y + (v - Y) * (1.0 + sa / 100.0 * a);
            }
        }
    }
    if (skinOn != 0) {
        vec3 hsv = rgbToHsv(clamp(v, 0.0, 1.0));
        float hh = hsv.x, ss = hsv.y, vv = hsv.z;
        float m = skinMask(hh, ss, vv);
        if (m > 0.0) {
            float uh = skUH / 100.0 * m, us = skUS / 100.0 * m;
            hh += hueDiff(skCH, hh) * uh;
            ss += (skCS - ss) * us * 0.8;
            hh += skHue * 0.15 * m;
            ss *= 1.0 + skSat / 100.0 * m;
            vv *= 1.0 + skLum / 100.0 * 0.3 * m;
            hh = mod(hh + 360.0, 360.0);
            ss = clamp(ss, 0.0, 1.0);
            vv = clamp(vv, 0.0, 1.0);
            v = hsvToRgb(hh, ss, vv);
        }
    }
    imageStore(outImg, px, vec4(clamp(v, 0.0, 1.0), 1.0));
}
)";

// 1 枚の画像から明るさ（と肌らしさ）を取り出す
const char* kPrep = R"(
layout(rgba32f, binding = 1) uniform readonly image2D img;
layout(r32f, binding = 2) uniform writeonly image2D outY;
layout(r32f, binding = 3) uniform writeonly image2D outM;
layout(r32f, binding = 4) uniform writeonly image2D outYM;
uniform ivec2 size;
uniform int withMask;
void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    vec3 c = imageLoad(img, px).rgb;
    float Y = dot(c, kLuma);
    imageStore(outY, px, vec4(Y));
    if (withMask != 0) {
        vec3 hsv = rgbToHsv(clamp(c, 0.0, 1.0));
        float m = skinMask(hsv.x, hsv.y, hsv.z);
        imageStore(outM, px, vec4(m));
        imageStore(outYM, px, vec4(Y * m));
    }
}
)";

// ガウスぼかし（1 方向）。端は CPU 版と同じく折り返し
const char* kBlur = R"(
layout(r32f, binding = 1) uniform readonly image2D src;
layout(r32f, binding = 2) uniform writeonly image2D dst;
layout(std430, binding = 3) readonly buffer K { float kern[]; };
uniform ivec2 size;
uniform int radius;
uniform int vertical;
void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    int n = vertical != 0 ? size.y : size.x;
    int i = vertical != 0 ? px.y : px.x;
    float acc = 0.0;
    for (int j = -radius; j <= radius; ++j) {
        int idx = i + j;
        if (idx < 0) idx = -idx;
        if (idx >= n) idx = 2 * n - 2 - idx;
        idx = clamp(idx, 0, n - 1);
        ivec2 q = vertical != 0 ? ivec2(px.x, idx) : ivec2(idx, px.y);
        acc += kern[j + radius] * imageLoad(src, q).r;
    }
    imageStore(dst, px, vec4(acc));
}
)";

// 肌の均一化（明るさ）・なめらかさ
const char* kSkinCombine = R"(
layout(rgba32f, binding = 1) uniform image2D img;
layout(r32f, binding = 2) uniform readonly image2D Yp;
layout(r32f, binding = 3) uniform readonly image2D Mp;
layout(r32f, binding = 4) uniform readonly image2D YmL;
layout(r32f, binding = 5) uniform readonly image2D MbL;
layout(r32f, binding = 6) uniform readonly image2D YmS;
layout(r32f, binding = 7) uniform readonly image2D MbS;
uniform ivec2 size;
uniform float uni, sm, keepFine;
void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    float m = imageLoad(Mp, px).r;
    if (m <= 0.0) return;
    float Y = imageLoad(Yp, px).r;
    float mbl = imageLoad(MbL, px).r;
    float large = mbl > 1e-3 ? imageLoad(YmL, px).r / mbl : Y;
    float ny = Y;
    if (sm > 0.0) {
        float mbs = imageLoad(MbS, px).r;
        float small = mbs > 1e-3 ? imageLoad(YmS, px).r / mbs : Y;
        float fine = Y - small, mid = small - large;
        float edge = exp(-(mid * mid) / (0.05 * 0.05));
        ny = large + mid * (1.0 - 0.8 * sm * m * edge) + fine * (1.0 - sm * m * (1.0 - keepFine));
    }
    if (uni > 0.0) ny += (large - ny) * uni * m * 0.5;
    float d = ny - Y;
    vec3 c = imageLoad(img, px).rgb;
    imageStore(img, px, vec4(clamp(c + d, 0.0, 1.0), 1.0));
}
)";

// シャープ（アンシャープマスク＋エッジだけに効かせるマスク）
const char* kSharpen = R"(
layout(rgba32f, binding = 1) uniform image2D img;
layout(r32f, binding = 2) uniform readonly image2D Yp;
layout(r32f, binding = 3) uniform readonly image2D Bp;
uniform ivec2 size;
uniform float k, maskAmt;
float at(int x, int y) { return imageLoad(Bp, ivec2(clamp(x, 0, size.x - 1), clamp(y, 0, size.y - 1))).r; }
void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (px.x >= size.x || px.y >= size.y) return;
    int x = px.x, y = px.y;
    float detail = imageLoad(Yp, px).r - imageLoad(Bp, px).r;
    if (maskAmt > 0.0) {
        float gx = (at(x + 1, y - 1) + 2.0 * at(x + 1, y) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0 * at(x - 1, y) + at(x - 1, y + 1));
        float gy = (at(x - 1, y + 1) + 2.0 * at(x, y + 1) + at(x + 1, y + 1)) - (at(x - 1, y - 1) + 2.0 * at(x, y - 1) + at(x + 1, y - 1));
        float mag = sqrt(gx * gx + gy * gy);
        float thr = maskAmt * 0.25;
        detail *= clamp((mag - thr * 0.5) / max(thr, 1e-4), 0.0, 1.0);
    }
    vec3 c = imageLoad(img, px).rgb + k * detail;
    imageStore(img, px, vec4(clamp(c, 0.0, 1.0), 1.0));
}
)";

std::vector<float> gaussKernel(float sigma, int& r) {
    r = std::max(1, static_cast<int>(std::lround(sigma * 4)));
    std::vector<float> k(2 * r + 1);
    float s = 0;
    for (int i = -r; i <= r; ++i) s += (k[i + r] = std::exp(-0.5f * (i / sigma) * (i / sigma)));
    for (float& x : k) x /= s;
    return k;
}

}  // namespace

// ---------------------------------------------------------------- 本体

struct GlBackend::Impl {
    QThread thread;
    QObject* worker = nullptr;  // GL スレッドに住む
    QOffscreenSurface* surface = nullptr;
    QOpenGLContext* ctx = nullptr;
    QOpenGLFunctions_4_3_Core* gl = nullptr;
    std::mutex mu;  // 1 度に 1 つの仕事
    bool broken = false;
    GLuint progMain = 0, progPrep = 0, progBlur = 0, progSkin = 0, progSharp = 0;
    GLuint texIn = 0, texOut = 0, texBrush = 0;
    GLuint planes[8] = {};
    GLuint buf[6] = {};  // tab, hsl, skinTab, masks, show, kernel
    int w = 0, h = 0, brushLayers = 0;

    template <class F>
    void runOnGl(F&& f) {
        QMetaObject::invokeMethod(worker, std::forward<F>(f), Qt::BlockingQueuedConnection);
    }

    GLuint compile(const char* body, QString* err) {
        const std::string src = std::string(kCommon) + body;
        const GLuint sh = gl->glCreateShader(GL_COMPUTE_SHADER);
        const char* s = src.c_str();
        gl->glShaderSource(sh, 1, &s, nullptr);
        gl->glCompileShader(sh);
        GLint ok = 0;
        gl->glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[4096];
            gl->glGetShaderInfoLog(sh, sizeof log, nullptr, log);
            if (err) *err = QString::fromUtf8(log);
            gl->glDeleteShader(sh);
            return 0;
        }
        const GLuint pr = gl->glCreateProgram();
        gl->glAttachShader(pr, sh);
        gl->glLinkProgram(pr);
        gl->glDeleteShader(sh);
        gl->glGetProgramiv(pr, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            gl->glGetProgramInfoLog(pr, sizeof log, nullptr, log);
            if (err) *err = QString::fromUtf8(log);
            return 0;
        }
        return pr;
    }

    void ensureSize(int nw, int nh) {
        if (nw == w && nh == h && texIn) return;
        auto mk = [&](GLuint& t, GLenum internal) {
            if (t) gl->glDeleteTextures(1, &t);
            gl->glGenTextures(1, &t);
            gl->glBindTexture(GL_TEXTURE_2D, t);
            gl->glTexStorage2D(GL_TEXTURE_2D, 1, internal, nw, nh);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        };
        mk(texIn, GL_RGB32F);
        mk(texOut, GL_RGBA32F);
        for (GLuint& p : planes) mk(p, GL_R32F);
        w = nw;
        h = nh;
    }

    void setBuffer(int i, const void* data, size_t bytes) {
        if (!buf[i]) gl->glGenBuffers(1, &buf[i]);
        gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[i]);
        gl->glBufferData(GL_SHADER_STORAGE_BUFFER, std::max<size_t>(bytes, 16), data, GL_DYNAMIC_DRAW);
    }

    void dispatch() {
        gl->glDispatchCompute((w + 15) / 16, (h + 15) / 16, 1);
        gl->glMemoryBarrier(GL_ALL_BARRIER_BITS);
    }

    void blur(GLuint src, GLuint dst, GLuint tmp, float sigma) {
        if (sigma <= 0.05f) {
            gl->glCopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0, dst, GL_TEXTURE_2D, 0, 0, 0, 0, w, h, 1);
            return;
        }
        int r = 0;
        const auto k = gaussKernel(sigma, r);
        setBuffer(5, k.data(), k.size() * 4);
        gl->glUseProgram(progBlur);
        gl->glUniform2i(gl->glGetUniformLocation(progBlur, "size"), w, h);
        gl->glUniform1i(gl->glGetUniformLocation(progBlur, "radius"), r);
        gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, buf[5]);
        gl->glUniform1i(gl->glGetUniformLocation(progBlur, "vertical"), 0);
        gl->glBindImageTexture(1, src, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        gl->glBindImageTexture(2, tmp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        dispatch();
        gl->glUniform1i(gl->glGetUniformLocation(progBlur, "vertical"), 1);
        gl->glBindImageTexture(1, tmp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
        gl->glBindImageTexture(2, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
        dispatch();
    }

    void skinUniforms(GLuint prog, const Params& p) {
        gl->glUseProgram(prog);
        auto u = [&](const char* n, float v) { gl->glUniform1f(gl->glGetUniformLocation(prog, n), v); };
        u("skCH", p.skinCenterHue);
        u("skCS", p.skinCenterSat);
        u("skRange", p.skinRange);
        u("skHue", p.skinHue);
        u("skSat", p.skinSat);
        u("skLum", p.skinLum);
        u("skUH", p.skinUniHue);
        u("skUS", p.skinUniSat);
    }

    bool run(const MainStage& st, ImageF& out) {
        const ImageF& lin = *st.lin;
        const Params& p = *st.p;
        const int nMask = st.masks ? st.masks->count() : 0;
        if (nMask > 16) return false;
        GLint maxTex = 0;
        gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        if (lin.width > maxTex || lin.height > maxTex) return false;
        while (gl->glGetError() != GL_NO_ERROR) {
        }
        ensureSize(lin.width, lin.height);
        // 入力
        gl->glBindTexture(GL_TEXTURE_2D, texIn);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_FLOAT, lin.data.data());
        // 表
        const int tabN = int((*st.tab)[0].size());
        std::vector<float> tab(size_t(tabN) * 3);
        for (int c = 0; c < 3; ++c) std::copy((*st.tab)[c].begin(), (*st.tab)[c].end(), tab.begin() + size_t(c) * tabN);
        setBuffer(0, tab.data(), tab.size() * 4);
        std::vector<float> hslT(721 * 3, 0.0f);
        if (st.hsl) {
            std::copy(st.tHue->begin(), st.tHue->end(), hslT.begin());
            std::copy(st.tSat->begin(), st.tSat->end(), hslT.begin() + 721);
            std::copy(st.tLum->begin(), st.tLum->end(), hslT.begin() + 1442);
        }
        setBuffer(1, hslT.data(), hslT.size() * 4);
        setBuffer(2, st.skinTab->data(), st.skinTab->size() * 4);
        // マスク
        std::vector<float> mk(size_t(std::max(1, nMask)) * 20, 0.0f);
        int layers = 0;
        for (int i = 0; i < nMask; ++i) {
            const LocalMask& m = st.masks->mask(i);
            float* q = &mk[size_t(i) * 20];
            q[0] = m.type == "linear" ? 0 : m.type == "radial" ? 1 : m.type == "brush" ? 2 : m.type == "skin" ? 3 : 4;
            q[1] = m.invert;
            q[2] = m.enabled;
            q[3] = m.type == "brush" ? float(layers++) : 0;
            q[4] = m.x0; q[5] = m.y0; q[6] = m.x1; q[7] = m.y1; q[8] = m.angle; q[9] = m.feather;
            q[10] = m.exposure; q[11] = m.contrast; q[12] = m.highlights; q[13] = m.shadows;
            q[14] = m.temp; q[15] = m.tint; q[16] = m.saturation;
        }
        setBuffer(3, mk.data(), mk.size() * 4);
        // ブラシ（層ごとに）
        const int L = std::max(1, layers);
        if (!texBrush || brushLayers != L || layers > 0) {
            if (texBrush) gl->glDeleteTextures(1, &texBrush);
            gl->glGenTextures(1, &texBrush);
            gl->glBindTexture(GL_TEXTURE_2D_ARRAY, texBrush);
            gl->glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R32F, layers ? w : 1, layers ? h : 1, L);
            gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            brushLayers = L;
            int li = 0;
            for (int i = 0; i < nMask; ++i)
                if (st.masks->mask(i).type == "brush") {
                    const auto& bp = st.masks->brushPlane(i);
                    if (!bp.empty()) gl->glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, li, w, h, 1, GL_RED, GL_FLOAT, bp.data());
                    ++li;
                }
        }
        const bool show = st.showSlot >= 0 && st.showVals;
        setBuffer(4, nullptr, show ? size_t(w) * h * 4 : 16);

        // ---- 1 画素ごとの処理
        gl->glUseProgram(progMain);
        skinUniforms(progMain, p);
        auto ui = [&](const char* n, int v) { gl->glUniform1i(gl->glGetUniformLocation(progMain, n), v); };
        auto uf = [&](const char* n, float v) { gl->glUniform1f(gl->glGetUniformLocation(progMain, n), v); };
        gl->glUniform2i(gl->glGetUniformLocation(progMain, "size"), w, h);
        gl->glUniform3f(gl->glGetUniformLocation(progMain, "gains"), st.gains[0], st.gains[1], st.gains[2]);
        ui("camMat", st.camMat);
        gl->glUniform1fv(gl->glGetUniformLocation(progMain, "CM"), 9, p.camMatrix.data());
        ui("tabN", tabN);
        uf("sScale", st.sScale);
        uf("satAmt", st.sat);
        uf("vibAmt", st.vib);
        ui("hslOn", st.hsl);
        ui("hasHue", st.hasHue);
        ui("hasSat", st.hasSat);
        ui("hasLum", st.hasLum);
        ui("skinOn", st.skin);
        ui("nMask", nMask);
        ui("showSlot", show ? st.showSlot : -1);
        gl->glUniform4f(gl->glGetUniformLocation(progMain, "mapv"), float(st.map.nx0), float(st.map.ny0), float(st.map.dx), float(st.map.dy));
        uf("asp", float(st.map.fullH / st.map.fullW));
        ui("inLin", 0);
        ui("brushTex", 7);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, texIn);
        gl->glActiveTexture(GL_TEXTURE7);
        gl->glBindTexture(GL_TEXTURE_2D_ARRAY, texBrush);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindImageTexture(1, texOut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);
        for (int i = 0; i < 5; ++i) gl->glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2 + i, buf[i]);
        dispatch();

        // ---- 肌の均一化・なめらかさ
        if (st.skinSpatial) {
            GLuint Yp = planes[0], Mp = planes[1], YMp = planes[2], tmp = planes[3], YmL = planes[4], MbL = planes[5], YmS = planes[6],
                   MbS = planes[7];
            skinUniforms(progPrep, p);
            gl->glUniform2i(gl->glGetUniformLocation(progPrep, "size"), w, h);
            gl->glUniform1i(gl->glGetUniformLocation(progPrep, "withMask"), 1);
            gl->glBindImageTexture(1, texOut, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
            gl->glBindImageTexture(2, Yp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            gl->glBindImageTexture(3, Mp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            gl->glBindImageTexture(4, YMp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            dispatch();
            const float sL = std::max(1.0f, 14.0f * st.scale), sS = std::max(0.5f, 1.5f * st.scale);
            blur(YMp, YmL, tmp, sL);
            blur(Mp, MbL, tmp, sL);
            if (p.skinSmooth > 0) {
                blur(YMp, YmS, tmp, sS);
                blur(Mp, MbS, tmp, sS);
            }
            skinUniforms(progSkin, p);
            gl->glUniform2i(gl->glGetUniformLocation(progSkin, "size"), w, h);
            gl->glUniform1f(gl->glGetUniformLocation(progSkin, "uni"), p.skinUniLum / 100.0f * 0.85f);
            gl->glUniform1f(gl->glGetUniformLocation(progSkin, "sm"), p.skinSmooth / 100.0f);
            gl->glUniform1f(gl->glGetUniformLocation(progSkin, "keepFine"), 0.4f + 0.6f * p.skinTexture / 100.0f);
            gl->glBindImageTexture(1, texOut, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            const GLuint ins[6] = {Yp, Mp, YmL, MbL, YmS, MbS};
            for (int i = 0; i < 6; ++i) gl->glBindImageTexture(2 + i, ins[i], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            dispatch();
        }
        // ---- シャープ
        if (st.detail) {
            GLuint Yp = planes[0], Bp = planes[1], tmp = planes[3];
            gl->glUseProgram(progPrep);
            gl->glUniform2i(gl->glGetUniformLocation(progPrep, "size"), w, h);
            gl->glUniform1i(gl->glGetUniformLocation(progPrep, "withMask"), 0);
            gl->glBindImageTexture(1, texOut, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
            gl->glBindImageTexture(2, Yp, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            gl->glBindImageTexture(3, planes[6], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            gl->glBindImageTexture(4, planes[7], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
            dispatch();
            blur(Yp, Bp, tmp, p.sharpenRadius * st.scale);
            gl->glUseProgram(progSharp);
            gl->glUniform2i(gl->glGetUniformLocation(progSharp, "size"), w, h);
            gl->glUniform1f(gl->glGetUniformLocation(progSharp, "k"), p.sharpenAmount / 100.0f * 1.2f);
            gl->glUniform1f(gl->glGetUniformLocation(progSharp, "maskAmt"), p.sharpenMasking / 100.0f);
            gl->glBindImageTexture(1, texOut, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
            gl->glBindImageTexture(2, Yp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            gl->glBindImageTexture(3, Bp, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32F);
            dispatch();
        }
        // ---- 結果を受け取る
        out.resize(w, h);
        gl->glBindTexture(GL_TEXTURE_2D, texOut);
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
        gl->glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, out.data.data());
        if (show) {
            st.showVals->resize(size_t(w) * h);
            gl->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf[4]);
            gl->glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, size_t(w) * h * 4, st.showVals->data());
        }
        const GLenum e = gl->glGetError();
        if (e != GL_NO_ERROR) {
            std::fprintf(stderr, "GPU: GL error 0x%x\n", e);
            return false;
        }
        return true;
    }
};

GlBackend* GlBackend::create(bool allowSoftware, QString* why) {
    auto* b = new GlBackend;
    b->d_ = std::make_unique<Impl>();
    Impl& d = *b->d_;
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(4, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    d.surface = new QOffscreenSurface;
    d.surface->setFormat(fmt);
    d.surface->create();
    if (!d.surface->isValid()) {
        if (why) *why = QStringLiteral("画面外の描画先を作れませんでした");
        delete b;
        return nullptr;
    }
    d.worker = new QObject;
    d.worker->moveToThread(&d.thread);
    d.thread.setObjectName("RawDeveloper GPU");
    d.thread.start();
    bool ok = false;
    QString err, renderer;
    d.runOnGl([&] {
        d.ctx = new QOpenGLContext;
        d.ctx->setFormat(fmt);
        if (!d.ctx->create() || !d.ctx->makeCurrent(d.surface)) {
            err = QStringLiteral("OpenGL を使えませんでした");
            return;
        }
        const auto f = d.ctx->format();
        if (f.majorVersion() < 4 || (f.majorVersion() == 4 && f.minorVersion() < 3)) {
            err = QStringLiteral("OpenGL 4.3 以上が必要です（%1.%2）").arg(f.majorVersion()).arg(f.minorVersion());
            return;
        }
        d.gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_4_3_Core>(d.ctx);
        if (!d.gl || !d.gl->initializeOpenGLFunctions()) {
            err = QStringLiteral("OpenGL の関数を読み込めませんでした");
            return;
        }
        renderer = QString::fromLatin1(reinterpret_cast<const char*>(d.gl->glGetString(GL_RENDERER)));
        const QString low = renderer.toLower();
        if (!allowSoftware && (low.contains("llvmpipe") || low.contains("software") || low.contains("gdi generic") ||
                               low.contains("swiftshader") || low.contains("basic render"))) {
            err = QStringLiteral("ソフトウェア描画のため使いません（%1）").arg(renderer);
            return;
        }
        QString e;
        d.progMain = d.compile(kMain, &e);
        if (d.progMain) d.progPrep = d.compile(kPrep, &e);
        if (d.progPrep) d.progBlur = d.compile(kBlur, &e);
        if (d.progBlur) d.progSkin = d.compile(kSkinCombine, &e);
        if (d.progSkin) d.progSharp = d.compile(kSharpen, &e);
        if (!d.progSharp) {
            err = QStringLiteral("シェーダーを作れませんでした：") + e.left(400);
            return;
        }
        ok = true;
    });
    if (!ok) {
        if (why) *why = err;
        delete b;
        return nullptr;
    }
    b->renderer_ = renderer;
    return b;
}

GlBackend::~GlBackend() {
    if (!d_) return;
    if (d_->thread.isRunning()) {
        d_->runOnGl([this] {
            if (d_->ctx) {
                d_->ctx->doneCurrent();
                delete d_->ctx;
                d_->ctx = nullptr;
            }
        });
        d_->thread.quit();
        d_->thread.wait();
    }
    delete d_->worker;
    delete d_->surface;
}

bool GlBackend::runMain(const MainStage& st, ImageF& out) {
    if (!d_ || d_->broken) return false;
    if (!st.lin || st.lin->width < 1 || st.lin->height < 1) return false;  // 空の画像は CPU で（GPU を止めない）
    std::lock_guard<std::mutex> lk(d_->mu);
    bool ok = false;
    d_->runOnGl([&] { ok = d_->run(st, out); });
    if (!ok) d_->broken = true;  // 一度失敗したら以後は CPU で
    return ok;
}

}  // namespace rd
