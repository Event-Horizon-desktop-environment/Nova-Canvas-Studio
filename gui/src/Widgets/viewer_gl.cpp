#include "viewer_gl.hpp"
#include "Logging.hpp"

#include "canvas/core/gpu/colorspace.hpp"

#include <QImage>
#include <QOpenGLContext>
#include <QPainter>
#include <QVector2D>

#include <array>
#include <algorithm>
#include <cmath>

namespace canvas::gui {

namespace {

constexpr const char* kVertexSrc = R"(
#version 330 core
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
out vec2 v_uv;
void main() {
    v_uv = in_uv;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
)";

// NV12 -> RGB conversion (BT.601 limited range), matching the coefficients the
// CUDA composite kernel uses (frame space: R=Y+1.402Cr, G=Y-0.344U-0.714V,
// B=Y+1.772U with Y/U/V centered at 0; the 16..235 luma headroom is unwound by
// the 1.164 scale below). u_tex_y samples the R8 luma plane, u_tex_uv the
// interleaved CbCr plane (.r = Cb, .g = Cr).
constexpr const char* kFragNv12Src = R"(
#version 330 core
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    float Y  = texture(u_tex_y,  v_uv).r * 255.0;
    float Cb = texture(u_tex_uv, v_uv).r * 255.0 - 128.0;
    float Cr = texture(u_tex_uv, v_uv).g * 255.0 - 128.0;
    float r = 1.164 * (Y - 16.0) + 1.596 * Cr;
    float g = 1.164 * (Y - 16.0) - 0.392 * Cb - 0.813 * Cr;
    float b = 1.164 * (Y - 16.0) + 2.017 * Cb;
    fragColor = vec4(clamp(r, 0.0, 255.0),
                     clamp(g, 0.0, 255.0),
                     clamp(b, 0.0, 255.0), 1.0) / 255.0;
}
)";

constexpr const char* kFragSrc = R"(
#version 330 core
uniform sampler2D u_tex;      // outgoing (A)
uniform sampler2D u_tex_b;    // incoming (B), during a transition
uniform int    u_mode;        // TransitionRenderMode
uniform float  u_progress;    // 0..1 transition progress
uniform float  u_aspect;      // texture aspect (w/h) for circular/wipe shapes
in vec2 v_uv;
out vec4 fragColor;

// mode constants (must match TransitionRenderMode in frame.hpp)
const int MODE_NONE         = 0;
const int MODE_CROSSDISS    = 1;
const int MODE_DIPBLACK     = 2;
const int MODE_FADEOUT      = 3;
const int MODE_FADEIN       = 4;
const int MODE_WIPELEFT     = 5;
const int MODE_WIPERIGHT    = 6;
const int MODE_WIPEUP       = 7;
const int MODE_WIPEDOWN     = 8;
const int MODE_FADEIN_A     = 9;   // single-clip: fade the A texture itself in from black

void main() {
    vec4 a = texture(u_tex, v_uv);
    if (u_mode == MODE_NONE) {
        fragColor = a;
        return;
    }
    vec4 b = texture(u_tex_b, v_uv);
    float t = clamp(u_progress, 0.0, 1.0);

    if (u_mode == MODE_FADEIN_A) {
        // Single-clip IN fade: blend A in from black (no second texture).
        fragColor = a * t;
        return;
    }

    if (u_mode == MODE_CROSSDISS) {
        fragColor = mix(a, b, t);
        return;
    }
    if (u_mode == MODE_DIPBLACK) {
        // A fades out to black (first half), B fades in from black (second half).
        float phase = t < 0.5 ? (2.0 * t) : 1.0;
        vec4 black = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 first = mix(a, black, phase);
        if (t < 0.5) { fragColor = first; return; }
        vec4 second = mix(black, b, 2.0 * (t - 0.5));
        fragColor = second;
        return;
    }
    if (u_mode == MODE_FADEOUT) {
        fragColor = a * (1.0 - t);
        return;
    }
    if (u_mode == MODE_FADEIN) {
        fragColor = b * t;
        return;
    }

    // Wipes sweep B over A based on direction.
    vec2 uv = v_uv;
    float edge;
    if (u_mode == MODE_WIPELEFT)  edge = 1.0 - t;              // reveal from right, moving left
    else if (u_mode == MODE_WIPERIGHT) edge = t;               // reveal from left, moving right
    else if (u_mode == MODE_WIPEUP)   edge = 1.0 - t;          // reveal from bottom, moving up
    else edge = t;                                             // WIPE_DOWN: reveal from top, moving down

    // Convert UV to an axis where 0 = shown A fully, 1 = shown B fully. The
    // diagonal (for left/right) gives a soft-edged wipe; vertical likewise.
    float c;
    if (u_mode == MODE_WIPELEFT || u_mode == MODE_WIPERIGHT) c = uv.x;
    else c = uv.y;
    // Soft edge feather (2% of dimension) to hide hard aliasing seams.
    float feather = 0.02;
    float blend = smoothstep(edge - feather, edge + feather, c);
    fragColor = mix(a, b, blend);
    return;
}
)";

}  // namespace

ViewerGL::ViewerGL(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(320, 180);
    setMouseTracking(true);
}

void ViewerGL::set_frame(canvas::core::RenderFramePtr frame) {
    if (!frame) return;
    const bool has_rgba = frame->a && !frame->a->rgba.empty();
    const bool has_nv12 = frame->nv12 && !frame->nv12->y.empty();
    if (!has_rgba && !has_nv12) return;
    if (debug_enabled())
        qDebug() << "viewer: set_frame"
                 << "a_frame=" << (frame->a ? frame->a->frame_number : -1)
                 << "nv12_frame=" << (frame->nv12 ? frame->nv12->frame_number : -1)
                 << (frame->b ? "b_frame=" + QString::number(frame->b->frame_number) : QString())
                 << "size=" << (frame->a ? frame->a->width : (frame->nv12 ? frame->nv12->width : 0))
                 << "x"
                 << (frame->a ? frame->a->height : (frame->nv12 ? frame->nv12->height : 0))
                 << "progress=" << frame->progress
                 << "ctx_valid=" << (QOpenGLContext::currentContext() != nullptr);

    // Always-on viewer diagnostic (qWarning so the default handler keeps it):
    // frame pixel size vs widget size, which display path (RGBA vs GPU NV12),
    // and whether a small preview frame needs the software upscale.
    static int64_t viewer_log_ = 0;
    if ((viewer_log_++ % 16) == 0) {
        const int fw = (frame->a ? frame->a->width
                                 : (frame->nv12 ? frame->nv12->width : 0));
        const int fh = (frame->a ? frame->a->height
                                 : (frame->nv12 ? frame->nv12->height : 0));
        const int nw = (frame->nv12 ? frame->nv12->width : 0);
        const int nh = (frame->nv12 ? frame->nv12->height : 0);
        if (fw > 0 && fh > 0)
            qWarning() << "[viewer] set_frame"
                       << "frame=" << fw << "x" << fh
                       << "nv12=" << nw << "x" << nh
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height())
                       << "path=" << (frame->nv12 && !frame->nv12->y.empty() ? "nv12" : "rgba")
                       << "small=" << ((fw < width() || fh < height()) ? "yes" : "no")
                       << "last_tex=" << tex_w_ << "x" << tex_h_;
    }
    frame_ = std::move(frame);
    // Do NOT touch GL here: this is called from the GUI thread outside a
    // current QOpenGLWidget context. Defer the upload to paintGL, which runs
    // with the context current.
    texture_dirty_ = true;
    update();
}

void ViewerGL::clear() {
    frame_.reset();
    texture_valid_ = false;
    texture_second_valid_ = false;
    nv12_valid_ = false;
    texture_dirty_ = false;
    update();
}

void ViewerGL::set_mode(ViewerMode mode) {
    mode_ = mode;
    update();
}

void ViewerGL::set_scale_mode(ScaleMode mode) {
    scale_mode_ = mode;
    update();
}

void ViewerGL::initializeGL() {
    initializeOpenGLFunctions();

    program_ = std::make_unique<QOpenGLShaderProgram>();
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragSrc);
    program_->link();
    attr_pos_ = program_->attributeLocation("in_pos");
    attr_uv_ = program_->attributeLocation("in_uv");
    uni_mode_ = program_->uniformLocation("u_mode");
    uni_progress_ = program_->uniformLocation("u_progress");
    uni_aspect_ = program_->uniformLocation("u_aspect");

    // NV12 (GPU composite fast path) needs its own program: two samplers (Y +
    // interleaved CbCr) instead of one RGBA texture.
    program_nv12_ = std::make_unique<QOpenGLShaderProgram>();
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexSrc);
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragNv12Src);
    program_nv12_->link();

    // Unit quad covering NDC in [-1,1]; aspect/letterboxing is handled by
    // adjusting the quad positions each frame from the texture aspect.
    static const float kQuad[] = {
        // x     y     u     v
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kQuad, sizeof(kQuad));

    // Attribute layout is identical for both programs (same vertex layout and
    // shared VAO); we just bind the appropriate program on draw.
    vao_.create();
    vao_.bind();
    program_->enableAttributeArray(attr_pos_);
    program_->setAttributeBuffer(attr_pos_, GL_FLOAT, 0, 2, 4 * sizeof(float));
    program_->enableAttributeArray(attr_uv_);
    program_->setAttributeBuffer(attr_uv_, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    vao_.release();
    vbo_.release();

    texture_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_->setWrapMode(QOpenGLTexture::ClampToEdge);

    texture_b_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_b_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_b_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_b_->setWrapMode(QOpenGLTexture::ClampToEdge);

    texture_nv12_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_y_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_y_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_y_->setWrapMode(QOpenGLTexture::ClampToEdge);

    texture_nv12_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_uv_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);

    if (frame_) upload_frame();
}

void ViewerGL::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void ViewerGL::upload_frame() {
    if (!frame_ || !texture_) return;

    // NV12 GPU fast path: upload the two planes as R8 (luma) + RG8 (CbCr)
    // textures; the BT.601 YUV->RGB conversion is applied in the fragment
    // shader.
    if (frame_->nv12 && !frame_->nv12->y.empty()) {
        const canvas::core::Nv12Frame* n = frame_->nv12.get();
        const int w = n->width;
        const int h = n->height;

        // Small previews (scrub) don't magnify reliably in GL on some drivers, so
        // when the NV12 plane is smaller than the player, convert to CPU RGBA
        // and fall through to the RGBA path below. Full-res playback keeps the
        // fast NV12 texture path.
        if (w > 0 && h > 0 && (w < width() || h < height())) {
            static int64_t nv12cvt_log_ = 0;
            if ((nv12cvt_log_++ % 16) == 0)
                qWarning() << "[viewer] NV12->RGBA"
                           << "src=" << w << "x" << h
                           << "widget=" << std::max(1, width()) << "x" << std::max(1, height())
                           << "frame=" << (frame_->a ? frame_->a->frame_number : -1);
            auto rgba = std::make_shared<canvas::core::VideoFrame>();
            rgba->width = w;
            rgba->height = h;
            rgba->stride = static_cast<std::size_t>(w) * 4;
            rgba->rgba.assign(rgba->stride * static_cast<std::size_t>(h), 0);
            const int y_p = static_cast<int>(n->y_pitch);
            const int uv_p = static_cast<int>(n->uv_pitch);
            for (int y = 0; y < h; ++y) {
                const uint8_t* yrow = n->y.data() + static_cast<std::size_t>(y) * y_p;
                uint8_t* prow = rgba->rgba.data() + static_cast<std::size_t>(y) * rgba->stride;
                for (int x = 0; x < w; ++x) {
                    const int ux = x / 2, vy = y / 2;
                    const std::size_t uvoff = static_cast<std::size_t>(vy) * uv_p +
                                              static_cast<std::size_t>(ux) * 2;
                    const std::uint8_t cb = n->uv.data()[uvoff];
                    const std::uint8_t cr = n->uv.data()[uvoff + 1];
                    const auto rgb = canvas::core::gpu::yuv_to_rgb(yrow[x], cb, cr);
                    prow[x * 4 + 0] = rgb.r;
                    prow[x * 4 + 1] = rgb.g;
                    prow[x * 4 + 2] = rgb.b;
                    prow[x * 4 + 3] = 255;
                }
            }
            auto rf = std::make_shared<canvas::core::RenderFrame>();
            rf->a = std::move(rgba);
            rf->b = frame_->b;
            rf->mode = frame_->mode;
            rf->progress = frame_->progress;
            rf->fade_from_black = frame_->fade_from_black;
            rf->fade_to_black = frame_->fade_to_black;
            frame_ = std::move(rf);
            nv12_valid_ = false;
            return;   // no texture yet; paintGL calls upload_frame again for RGBA
        }

        const bool y_realloc = !texture_nv12_y_->isStorageAllocated() || tex_w_ != w ||
                               tex_h_ != h;
        if (y_realloc) {
            texture_nv12_y_->setSize(w, h);
            texture_nv12_y_->setFormat(QOpenGLTexture::R8_UNorm);
            texture_nv12_y_->allocateStorage();
            texture_nv12_uv_->setSize(w / 2, h / 2);
            texture_nv12_uv_->setFormat(QOpenGLTexture::RG8_UNorm);
            texture_nv12_uv_->allocateStorage();
            tex_w_ = w;
            tex_h_ = h;
        }
        // y_pitch is the tightly-packed stride; GL texture rows are also tightly
        // packed, so upload row by row only if the pitch differs.
        const int y_pitch = static_cast<int>(n->y_pitch);
        const int uv_pitch = static_cast<int>(n->uv_pitch);
        if (y_pitch == w && uv_pitch == w) {
            texture_nv12_y_->setData(0, 0, 0, w, h, 1, QOpenGLTexture::Red, QOpenGLTexture::UInt8,
                                     n->y.data());
            texture_nv12_uv_->setData(0, 0, 0, w / 2, h / 2, 1, QOpenGLTexture::RG,
                                      QOpenGLTexture::UInt8, n->uv.data());
        } else {
            for (int row = 0; row < h; ++row)
                texture_nv12_y_->setData(0, 0, row, w, 1, 1, QOpenGLTexture::Red,
                                         QOpenGLTexture::UInt8, n->y.data() + row * y_pitch);
            for (int row = 0; row < h / 2; ++row)
                texture_nv12_uv_->setData(0, 0, row, w / 2, 1, 1, QOpenGLTexture::RG,
                                          QOpenGLTexture::UInt8, n->uv.data() + row * uv_pitch);
        }
        texture_valid_ = false;
        texture_second_valid_ = false;
        nv12_valid_ = true;
        texture_dirty_ = false;
        static int64_t nv12up_log_ = 0;
        if ((nv12up_log_++ % 16) == 0)
            qWarning() << "[viewer] nv12_upload"
                       << "tex=" << w << "x" << h
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
        return;
    }

    nv12_valid_ = false;
    if (!frame_->a || frame_->a->rgba.empty()) {
        texture_dirty_ = false;
        return;
    }

    auto upload = [this](QOpenGLTexture& tex, const canvas::core::VideoFramePtr& f, int& tw, int& th,
                     bool& valid, const char* label) {
        if (!f || f->rgba.empty()) return;
        int w = f->width;
        int h = f->height;
        const uint8_t* data = f->rgba.data();
        std::size_t stride = f->stride;
        // Owned copy kept alive through setData() below whenever we upscale.
        QImage upscaled;

        // The scrub-preview texture is decoded small (640px) for speed; if the
        // player widget is larger, upscale in software so the frame fills the
        // media window even on drivers whose GL magnification misbehaves.
        // Aspect is preserved; the letterbox quad in paintGL then leaves at
        // most thin symmetrical black bars.
        const int vw = std::max(1, width());
        const int vh = std::max(1, height());
        if (w > 0 && h > 0 && (vw > w || vh > h) &&
            vw > 2 && vh > 2 && static_cast<std::size_t>(w) * h * 4ULL <= f->rgba.size()) {
            const double scale = std::min(static_cast<double>(vw) / w,
                                          static_cast<double>(vh) / h);
            int dw = std::max(1, static_cast<int>(std::llround(w * scale)));
            int dh = std::max(1, static_cast<int>(std::llround(h * scale)));
            if (dw != w || dh != h) {
                // Copy into an owned QImage (RGBA8888) and smooth-scale to fill.
                static int upscale_log_ = 0;
                if ((upscale_log_++ % 12) == 0)
                    qWarning() << "[viewer] UPSCALE"
                               << "src=" << w << "x" << h
                               << "dst=" << dw << "x" << dh
                               << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
                QImage src(w, h, QImage::Format_RGBA8888);
                const std::size_t dstride = static_cast<std::size_t>(src.bytesPerLine());
                const std::size_t row = std::min(static_cast<std::size_t>(stride),
                                                 static_cast<std::size_t>(dstride));
                for (int y = 0; y < h && y < src.height(); ++y)
                    std::memcpy(src.bits() + static_cast<std::size_t>(y) * dstride,
                                data + static_cast<std::size_t>(y) * stride, row);
                QImage dst = src.scaled(QSize(dw, dh), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                if (dst.width() == dw && dst.height() == dh &&
                    dst.format() == QImage::Format_RGBA8888) {
                    upscaled = std::move(dst);   // keep pixels alive for setData
                    w = dw;
                    h = dh;
                    data = upscaled.constBits();
                    stride = static_cast<std::size_t>(dw) * 4;
                }
            }
        }

        const bool realloc = !tex.isStorageAllocated() || tw != w || th != h;
        static int64_t tex_log_ = 0;
        if ((tex_log_++ % 16) == 0)
            qWarning() << "[viewer] rgba_upload"
                       << "tex=" << w << "x" << h
                       << "src_frame=" << f->width << "x" << f->height
                       << "upscaled=" << (w != f->width || h != f->height ? "yes" : "no")
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
        if (realloc) {
            tex.setSize(w, h);
            tex.setFormat(QOpenGLTexture::RGBA8_UNorm);
            tex.allocateStorage();
            tw = w;
            th = h;
        }
        tex.setData(0, 0, 0, w, h, 1, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, data);
        valid = true;
    };

    upload(*texture_, frame_->a, tex_w_, tex_h_, texture_valid_, "tex_a");

    texture_second_valid_ = false;
    if (frame_->b && frame_->b->rgba.size() >= frame_->b->stride * frame_->b->height) {
        int bw = 0;
        int bh = 0;
        upload(*texture_b_, frame_->b, bw, bh, texture_second_valid_, "tex_b");
        // Keep second texture size so letterboxing matches texture A's aspect.
    }

    texture_dirty_ = false;
}

void ViewerGL::paintGL() {
    // Ensure the GL viewport tracks the widget's physical size. Qt normally
    // calls resizeGL() on widget resize, but on some platforms the buffer can
    // lag behind (especially with rapid scrub updates) and content would render
    // into a small top-left box. Re-asserting here is idempotent. On HiDPI the
    // framebuffer is sized in *device* pixels, so `width()` isn't enough —
    // multiply by the pixel ratio or content renders at a quarter resolution.
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, std::max(1, static_cast<int>(std::lround(width() * dpr))),
               std::max(1, static_cast<int>(std::lround(height() * dpr))));

    const QColor bg(10, 10, 12);
    glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (debug_enabled() && texture_dirty_)
        qDebug() << "viewer: paintGL dirty, uploading frame"
                 << (frame_ && frame_->nv12 ? frame_->nv12->frame_number
                    : (frame_ && frame_->a ? frame_->a->frame_number : -1));

    const bool has_frame_texture = texture_valid_ || nv12_valid_;
    if (!has_frame_texture) {
        // Upload a pending frame here, on the context thread, so the very first
        // frame also shows without prior GL calls from outside paintGL.
        if (texture_dirty_) upload_frame();
        if (!(texture_valid_ || nv12_valid_)) {
            draw_blank();
            return;
        }
    } else if (texture_dirty_) {
        upload_frame();
    }

    // Letterbox into the viewport keeping the aspect ratio. Log the exact
    // projected draw-size (1.0 = full media window) so a default session
    // captures the real fill; always-on (qWarning), throttled to ~1 line/sec.
    const float vw = static_cast<float>(width());
    const float vh = static_cast<float>(height());
    const float aspect = tex_h_ > 0 ? static_cast<float>(tex_w_) / tex_h_ : 1.0f;
    const float va = vw / vh;

    // Letterbox: Fit (default) shows the whole frame with bars on the odd axis;
    // Fill covers the window edge-to-edge by cropping the overflow axis. Both
    // preserve pixel aspect (qw/qh scale the quad as a whole):
    //   Fit : qw,qh <= 1  (quad inside screen)
    //   Fill: qw,qh >= 1  (quad covers screen, texture cropped at the edge)
    const float qw = (scale_mode_ == ScaleMode::Fill)
                         ? std::max(aspect / va, 1.0f)
                         : std::min(aspect / va, 1.0f);
    const float qh = (scale_mode_ == ScaleMode::Fill)
                         ? std::max(va / aspect, 1.0f)
                         : std::min(va / aspect, 1.0f);
    {
        static int64_t paint_log_ = 0;
        if ((paint_log_++ % 30) == 0)
            qWarning() << "[viewer] paint"
                       << "mode=" << (scale_mode_ == ScaleMode::Fill ? "fill" : "fit")
                       << "tex=" << tex_w_ << "x" << tex_h_
                       << "widget=" << static_cast<int>(vw) << "x" << static_cast<int>(vh)
                       << "tex_aspect=" << aspect
                       << "widget_aspect=" << va
                       << "cover_x=" << qw << "cover_y=" << qh
                       << "path=" << (nv12_valid_ ? "nv12" : "rgba");
    }

    // Single-clip edge fade (fade-in-from-black at the clip's head, or
    // fade-out-to-black at its tail) blends the A texture against black via
    // u_mode/u_progress. It must use the RGBA path (the NV12 fast path can't
    // apply the fade) and needs no B texture.
    const bool single_fade = frame_ && (frame_->fade_from_black || frame_->fade_to_black);

    const bool use_nv12 = !single_fade && nv12_valid_ && frame_ && frame_->nv12;
    if (use_nv12) {
        program_nv12_->bind();
        vao_.bind();
        texture_nv12_y_->bind(0);
        texture_nv12_uv_->bind(1);
        program_nv12_->setUniformValue("u_tex_y", 0);
        program_nv12_->setUniformValue("u_tex_uv", 1);
    } else {
        program_->bind();
        vao_.bind();
        texture_->bind(0);
        program_->setUniformValue("u_tex", 0);

        if (single_fade) {
            // Fade the A texture against black. Bind A to both slots so the shader's B
            // texture() is valid even though the fade modes don't use it.
            texture_->bind(1);
            program_->setUniformValue("u_tex_b", 1);
            const int fade_mode = frame_->fade_from_black ? 9 /*MODE_FADEIN_A*/
                                                          : 3 /*MODE_FADEOUT*/;
            program_->setUniformValue("u_mode", fade_mode);
            program_->setUniformValue("u_progress", frame_->progress);
        } else {
            const bool trans = texture_second_valid_ && frame_ && frame_->has_transition();
            if (trans) {
                texture_b_->bind(1);
                program_->setUniformValue("u_tex_b", 1);
            }
            program_->setUniformValue("u_mode", trans ? static_cast<int>(frame_->mode) : 0);
            program_->setUniformValue("u_progress", trans ? frame_->progress : 0.0f);
        }
        program_->setUniformValue("u_aspect", aspect);
    }

    // Scale the unit quad's X/Y by the letterbox factor by re-buffering the
    // quad positions (UVs unchanged); the GPU scales during raster.
    static const std::array<float, 16> s_src = {
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    std::array<float, 16> s = s_src;
    const bool tf = frame_ && (frame_->scale_x != 1.0f || frame_->scale_y != 1.0f ||
                               frame_->pos_x != 0.0 || frame_->pos_y != 0.0 ||
                               frame_->rotation_deg != 0.0f ||
                               frame_->anchor_dx != 0.0 || frame_->anchor_dy != 0.0 ||
                               frame_->flip_h || frame_->flip_v);
    if (tf) {
        // Math in output-pixel space, mirroring the exporter's mapping so the
        // viewport and an export agree: pivot P = fitted-rect center + anchor;
        // scale about P, mirror (flips), rotate about P, then translate by the
        // pixel position. Corners are the fitted letterbox rect in screen px.
        const double hw = qw * vw * 0.5;
        const double hh = qh * vh * 0.5;
        const double cx = vw * 0.5;
        const double cy = vh * 0.5;
        const double px = cx + frame_->anchor_dx;
        const double py = cy + frame_->anchor_dy;
        const double ang = frame_->rotation_deg * 3.14159265358979323846 / 180.0;
        const double cs = std::cos(ang);
        const double sn = std::sin(ang);
        const double sx = frame_->scale_x;
        const double sy = frame_->scale_y;
        const double fxx = frame_->flip_h ? -1.0 : 1.0;
        const double fyy = frame_->flip_v ? -1.0 : 1.0;
        const double corners[4][2] = {
            {-hw, -hh}, {hw, -hh}, {-hw, hh}, {hw, hh}};
        for (int i = 0; i < 16; i += 4) {
            const int k = i / 4;
            const double bx = corners[k][0] + cx - px;  // v - P
            const double by = corners[k][1] + cy - py;
            const double ax = bx * sx * fxx;
            const double ay = by * sy * fyy;
            const double rx = ax * cs - ay * sn;
            const double ry = ax * sn + ay * cs;
            const double ox = px + rx + frame_->pos_x;
            const double oy = py + ry + frame_->pos_y;
            s[i] = static_cast<float>(2.0 * ox / vw - 1.0);
            s[i + 1] = static_cast<float>(1.0 - 2.0 * oy / vh);
            if (frame_->flip_h) s[i + 2] = 1.0f - s[i + 2];
            if (frame_->flip_v) s[i + 3] = 1.0f - s[i + 3];
        }
    } else {
        for (int i = 0; i < 16; i += 4) {
            s[i] *= qw;       // x
            s[i + 1] *= qh;   // y
        }
    }
    vbo_.bind();
    vbo_.write(0, s.data(), sizeof(float) * s.size());

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (use_nv12) {
        texture_nv12_uv_->release();
        texture_nv12_y_->release();
        program_nv12_->release();
    } else {
        if (single_fade) {
            texture_->release();  // bound to both slots 0/1
        } else if (texture_second_valid_ && frame_ && frame_->has_transition()) {
            texture_b_->release();
        }
        texture_->release();
        program_->release();
    }
    vao_.release();
    vbo_.release();
}

void ViewerGL::draw_blank() {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(10, 10, 12));
    painter.setPen(QPen(QColor(0x2A, 0x2F, 0x3C), 1.0));
    painter.setBrush(QColor(0x1A, 0x1D, 0x27));
    const QRectF badge(4, 4, 64, 16);
    painter.drawRoundedRect(badge, 8, 8);
    painter.setPen(QColor(0x9A, 0xA0, 0xB0));
    QFont f = painter.font();
    f.setPointSizeF(8);
    painter.setFont(f);
    painter.drawText(badge, Qt::AlignCenter, mode_ == ViewerMode::Source ? QStringLiteral("SOURCE")
                                                                          : QStringLiteral("PROGRAM"));
}

}