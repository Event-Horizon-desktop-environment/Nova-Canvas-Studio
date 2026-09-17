#include "viewer_gl.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"
#include "features/playback/vaapi_import_state.hpp"

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <QImage>
#include <QOpenGLContext>
#include <QPainter>
#include <QPainterPath>
#include <QVector2D>
#include <QLineF>
#include <QFontMetricsF>

#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <utility>

namespace canvas::gui {

namespace {

QByteArray viewer_shader_header(bool gles, bool fragment) {
    if (gles) {
        if (fragment)
            return QByteArray("#version 300 es\n"
                              "precision highp float;\n"
                              "precision highp int;\n"
                              "precision highp sampler2D;\n"
                              "precision highp sampler3D;\n");
        return QByteArray("#version 300 es\n"
                          "precision highp float;\n"
                          "precision highp int;\n");
    }
    return QByteArray("#version 330 core\n");
}

using TexImage3DProc = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                GLint, GLenum, GLenum, const void*);
TexImage3DProc tex_image_3d_proc() {
    static TexImage3DProc proc = [] {
        QOpenGLContext* c = QOpenGLContext::currentContext();
        return c ? reinterpret_cast<TexImage3DProc>(c->getProcAddress("glTexImage3D"))
                 : nullptr;
    }();
    return proc;
}

constexpr const char* kVertexSrc = R"(
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
out vec2 v_uv;
void main() {
    v_uv = in_uv;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
)";

void paint_checkerboard(QPainter& painter, const QRectF& area) {
    const QColor a(0x22, 0x22, 0x26);
    const QColor b(0x33, 0x33, 0x38);
    constexpr int kCell = 14;
    const int x0 = std::max(0, static_cast<int>(area.left()) - kCell);
    const int y0 = std::max(0, static_cast<int>(area.top()) - kCell);
    const int x1 = static_cast<int>(area.right()) + kCell;
    const int y1 = static_cast<int>(area.bottom()) + kCell;
    for (int y = y0; y < y1; y += kCell) {
        for (int x = x0; x < x1; x += kCell) {
            const bool even = (((x / kCell) + (y / kCell)) & 1) == 0;
            painter.fillRect(QRect(x, y, kCell, kCell), even ? a : b);
        }
    }
}

constexpr const char* kFragNv12Src = R"(
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;
uniform sampler3D u_grade;
uniform int       u_grade_size;
uniform int       u_matrix;
uniform int       u_range;
in vec2 v_uv;
out vec4 fragColor;

vec4 yuv_to_rgb(float Y, float Cb, float Cr, int matrix, int range) {
    float r_cr, g_cb, g_cr, b_cb;
    if (range == 1) {
        if (matrix == 0)      { r_cr = 1.402;   g_cb = -0.344;  g_cr = -0.714;  b_cb = 1.772; }
        else if (matrix == 2) { r_cr = 1.4746;  g_cb = -0.1645; g_cr = -0.5714; b_cb = 1.8814; }
        else                  { r_cr = 1.5748;  g_cb = -0.1873; g_cr = -0.4681; b_cb = 1.8556; }
    } else {
        if (matrix == 0)      { r_cr = 1.596;   g_cb = -0.392;  g_cr = -0.813;  b_cb = 2.017; }
        else if (matrix == 2) { r_cr = 1.679;   g_cb = -0.187;  g_cr = -0.650;  b_cb = 2.142; }
        else                  { r_cr = 1.793;   g_cb = -0.213;  g_cr = -0.533;  b_cb = 2.112; }
    }
    float Yl = (range == 1) ? Y : 1.164 * (Y - 16.0);
    float r = Yl + r_cr * Cr;
    float g = Yl + g_cb * Cb + g_cr * Cr;
    float b = Yl + b_cb * Cb;
    return vec4(clamp(r, 0.0, 255.0), clamp(g, 0.0, 255.0), clamp(b, 0.0, 255.0), 1.0) / 255.0;
}

void main() {
    float Y  = texture(u_tex_y,  v_uv).r * 255.0;
    float Cb = texture(u_tex_uv, v_uv).r * 255.0 - 128.0;
    float Cr = texture(u_tex_uv, v_uv).g * 255.0 - 128.0;
    vec3 rgb = yuv_to_rgb(Y, Cb, Cr, u_matrix, u_range).rgb;
    if (u_grade_size > 1) {
        vec3 coord = vec3(rgb.b, rgb.g, rgb.r) * float(u_grade_size - 1) / float(u_grade_size) + 0.5 / float(u_grade_size);
        rgb = texture(u_grade, coord).rgb;
    }
    fragColor = vec4(rgb, 1.0);
}
)";

constexpr const char* kFragNv12Trans = R"(
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;
uniform sampler2D u_tex_b_y;
uniform sampler2D u_tex_b_uv;
uniform sampler3D u_grade_a;
uniform sampler3D u_grade_b;
uniform int    u_grade_a_size;
uniform int    u_grade_b_size;
uniform int    u_matrix_a;
uniform int    u_range_a;
uniform int    u_matrix_b;
uniform int    u_range_b;
uniform int    u_mode;
uniform float  u_progress;
uniform float  u_aspect;
in vec2 v_uv;
out vec4 fragColor;

const int MODE_NONE         = 0;
const int MODE_CROSSDISS    = 1;
const int MODE_DIPBLACK     = 2;
const int MODE_FADEOUT      = 3;
const int MODE_FADEIN       = 4;
const int MODE_WIPELEFT     = 5;
const int MODE_WIPERIGHT    = 6;
const int MODE_WIPEUP       = 7;
const int MODE_WIPEDOWN     = 8;
const int MODE_FADEIN_A     = 9;

vec4 yuv_to_rgb(float Y, float Cb, float Cr, int matrix, int range) {
    float r_cr, g_cb, g_cr, b_cb;
    if (range == 1) {
        if (matrix == 0)      { r_cr = 1.402;   g_cb = -0.344;  g_cr = -0.714;  b_cb = 1.772; }
        else if (matrix == 2) { r_cr = 1.4746;  g_cb = -0.1645; g_cr = -0.5714; b_cb = 1.8814; }
        else                  { r_cr = 1.5748;  g_cb = -0.1873; g_cr = -0.4681; b_cb = 1.8556; }
    } else {
        if (matrix == 0)      { r_cr = 1.596;   g_cb = -0.392;  g_cr = -0.813;  b_cb = 2.017; }
        else if (matrix == 2) { r_cr = 1.679;   g_cb = -0.187;  g_cr = -0.650;  b_cb = 2.142; }
        else                  { r_cr = 1.793;   g_cb = -0.213;  g_cr = -0.533;  b_cb = 2.112; }
    }
    float Yl = (range == 1) ? Y : 1.164 * (Y - 16.0);
    float r = Yl + r_cr * Cr;
    float g = Yl + g_cb * Cb + g_cr * Cr;
    float b = Yl + b_cb * Cb;
    return vec4(clamp(r, 0.0, 255.0), clamp(g, 0.0, 255.0), clamp(b, 0.0, 255.0), 1.0) / 255.0;
}

vec4 sample_yuv(sampler2D ytex, sampler2D uvtex, vec2 p, int matrix, int range) {
    float Y  = texture(ytex,  p).r * 255.0;
    float Cb = texture(uvtex, p).r * 255.0 - 128.0;
    float Cr = texture(uvtex, p).g * 255.0 - 128.0;
    return yuv_to_rgb(Y, Cb, Cr, matrix, range);
}

vec4 grade_rgb(vec4 p, sampler3D lut, int size) {
    if (size < 2) return p;
    vec3 rgb = clamp(p.rgb, 0.0, 1.0);
    vec3 coord = vec3(rgb.b, rgb.g, rgb.r) * float(size - 1) / float(size) + 0.5 / float(size);
    return vec4(texture(lut, coord).rgb, p.a);
}

void main() {
    vec4 a = grade_rgb(sample_yuv(u_tex_y, u_tex_uv, v_uv, u_matrix_a, u_range_a), u_grade_a, u_grade_a_size);
    if (u_mode == MODE_NONE) { fragColor = a; return; }
    vec4 b = grade_rgb(sample_yuv(u_tex_b_y, u_tex_b_uv, v_uv, u_matrix_b, u_range_b), u_grade_b, u_grade_b_size);
    float t = clamp(u_progress, 0.0, 1.0);

    if (u_mode == MODE_FADEIN_A) { fragColor = a * t; return; }
    if (u_mode == MODE_CROSSDISS) { fragColor = mix(a, b, t); return; }
    if (u_mode == MODE_DIPBLACK) {
        float phase = t < 0.5 ? (2.0 * t) : 1.0;
        vec4 black = vec4(0.0, 0.0, 0.0, 1.0);
        vec4 first = mix(a, black, phase);
        if (t < 0.5) { fragColor = first; return; }
        fragColor = mix(black, b, 2.0 * (t - 0.5));
        return;
    }
    if (u_mode == MODE_FADEOUT) { fragColor = a * (1.0 - t); return; }
    if (u_mode == MODE_FADEIN) { fragColor = b * t; return; }

    vec2 uv = v_uv;
    float edge;
    if (u_mode == MODE_WIPELEFT)  edge = 1.0 - t;
    else if (u_mode == MODE_WIPERIGHT) edge = t;
    else if (u_mode == MODE_WIPEUP)   edge = 1.0 - t;
    else edge = t;
    float c;
    if (u_mode == MODE_WIPELEFT || u_mode == MODE_WIPERIGHT) c = uv.x;
    else c = uv.y;
    float feather = 0.02;
    float blend = smoothstep(edge - feather, edge + feather, c);
    fragColor = mix(a, b, blend);
    return;
}
)";

constexpr const char* kFragSrc = R"(
uniform sampler2D u_tex;
uniform sampler2D u_tex_b;
uniform sampler3D u_grade;
uniform sampler3D u_grade_b;
uniform int    u_grade_size;
uniform int    u_grade_b_size;
uniform int    u_mode;
uniform float  u_progress;
uniform float  u_aspect;
in vec2 v_uv;
out vec4 fragColor;

const int MODE_NONE         = 0;
const int MODE_CROSSDISS    = 1;
const int MODE_DIPBLACK     = 2;
const int MODE_FADEOUT      = 3;
const int MODE_FADEIN       = 4;
const int MODE_WIPELEFT     = 5;
const int MODE_WIPERIGHT    = 6;
const int MODE_WIPEUP       = 7;
const int MODE_WIPEDOWN     = 8;
const int MODE_FADEIN_A     = 9;

vec4 grade_rgb(vec4 p, sampler3D lut, int size) {
    if (size < 2) return p;
    vec3 rgb = clamp(p.rgb, 0.0, 1.0);
    vec3 coord = vec3(rgb.b, rgb.g, rgb.r) * float(size - 1) / float(size) + 0.5 / float(size);
    return vec4(texture(lut, coord).rgb, p.a);
}

void main() {
    vec4 a = grade_rgb(texture(u_tex, v_uv), u_grade, u_grade_size);
    if (u_mode == MODE_NONE) {
        fragColor = a;
        return;
    }
    vec4 b = grade_rgb(texture(u_tex_b, v_uv), u_grade_b, u_grade_b_size);
    float t = clamp(u_progress, 0.0, 1.0);

    if (u_mode == MODE_FADEIN_A) {
        fragColor = a * t;
        return;
    }

    if (u_mode == MODE_CROSSDISS) {
        fragColor = mix(a, b, t);
        return;
    }
    if (u_mode == MODE_DIPBLACK) {
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

    vec2 uv = v_uv;
    float edge;
    if (u_mode == MODE_WIPELEFT)  edge = 1.0 - t;
    else if (u_mode == MODE_WIPERIGHT) edge = t;
    else if (u_mode == MODE_WIPEUP)   edge = 1.0 - t;
    else edge = t;

    float c;
    if (u_mode == MODE_WIPELEFT || u_mode == MODE_WIPERIGHT) c = uv.x;
    else c = uv.y;
    float feather = 0.02;
    float blend = smoothstep(edge - feather, edge + feather, c);
    fragColor = mix(a, b, blend);
    return;
}
)";

}

ViewerGL::ViewerGL(QWidget* parent) : QOpenGLWidget(parent) {
    static uint64_t next_uid = 1;
    viewer_uid_ = next_uid++;
    setMinimumSize(320, 180);
    setMouseTracking(true);
}

ViewerGL::~ViewerGL() {
    if (context()) {
        makeCurrent();
        texture_.reset();
        texture_b_.reset();
        texture_nv12_y_.reset();
        texture_nv12_uv_.reset();
        texture_nv12_b_y_.reset();
        texture_nv12_b_uv_.reset();
        grade_tex_a_.reset();
        grade_tex_b_.reset();
        if (grade_neutral_tex_) glDeleteTextures(1, &grade_neutral_tex_);
        grade_neutral_tex_ = 0;
        if (vaapi_importer_) vaapi_importer_->release();
        if (vaapi_importer_b_) vaapi_importer_b_->release();
        program_.reset();
        program_nv12_.reset();
        program_nv12_trans_.reset();
        vbo_.destroy();
        vao_.destroy();
        doneCurrent();
    }
}

void ViewerGL::bind_nv12_a(const int y_unit, const int uv_unit) {
    if (vaapi_valid_) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + y_unit));
        glBindTexture(GL_TEXTURE_2D, vaapi_tex_y_);
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + uv_unit));
        glBindTexture(GL_TEXTURE_2D, vaapi_tex_uv_);
    } else {
        texture_nv12_y_->bind(y_unit);
        texture_nv12_uv_->bind(uv_unit);
    }
}

void ViewerGL::bind_nv12_b(const int y_unit, const int uv_unit) {
    if (vaapi_b_valid_) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + y_unit));
        glBindTexture(GL_TEXTURE_2D, vaapi_tex_b_y_);
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + uv_unit));
        glBindTexture(GL_TEXTURE_2D, vaapi_tex_b_uv_);
    } else {
        texture_nv12_b_y_->bind(y_unit);
        texture_nv12_b_uv_->bind(uv_unit);
    }
}

void ViewerGL::bind_grade_lut(const int unit, QOpenGLTexture* lut) {
    glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
    glBindTexture(GL_TEXTURE_3D, lut ? lut->textureId() : grade_neutral_tex_);
    glActiveTexture(GL_TEXTURE0);
}

void ViewerGL::set_frame(canvas::core::RenderFramePtr frame) {
    if (!frame) return;
    const bool has_rgba = frame->a && !frame->a->rgba.empty();
    const bool has_nv12 = frame->nv12 && (frame->nv12->has_cpu() || frame->nv12->has_gpu());
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

    const double recv_ms = have_last_arrival_
        ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                    last_frame_arrival_)
              .count()
        : 0.0;
    last_frame_arrival_ = std::chrono::steady_clock::now();
    have_last_arrival_ = true;
    static int64_t viewer_log_ = 0;
    if ((viewer_log_++ % 16) == 0) {
        const int fw = (frame->a ? frame->a->width
                                 : (frame->nv12 ? frame->nv12->width : 0));
        const int fh = (frame->a ? frame->a->height
                                 : (frame->nv12 ? frame->nv12->height : 0));
        const int nw = (frame->nv12 ? frame->nv12->width : 0);
        const int nh = (frame->nv12 ? frame->nv12->height : 0);
        ::canvas::core::log::log_warning(
            "[viewer] set_frame uid=%llu frame=%dx%d nv12=%dx%d widget=%dx%d "
            "path=%s small=%s last_tex=%dx%d recv_ms=%.1f",
            static_cast<unsigned long long>(viewer_uid_), fw, fh, nw, nh,
            std::max(1, width()), std::max(1, height()),
            frame->nv12 && frame->nv12->has_gpu()
                ? "vaapi"
                : (frame->nv12 && frame->nv12->has_cpu() ? "nv12" : "rgba"),
            (fw < width() || fh < height()) ? "yes" : "no", tex_w_, tex_h_, recv_ms);
        if (fw > 0 && fh > 0)
            qDebug() << "[viewer] set_frame"
                     << "frame=" << fw << "x" << fh
                       << "nv12=" << nw << "x" << nh
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height())
                       << "path=" << (frame->nv12 && frame->nv12->has_gpu()
                                          ? "vaapi"
                                          : (frame->nv12 && frame->nv12->has_cpu()
                                                 ? "nv12"
                                                 : "rgba"))
                       << "small=" << ((fw < width() || fh < height()) ? "yes" : "no")
                       << "last_tex=" << tex_w_ << "x" << tex_h_
                       << "recv_ms=" << QString::number(recv_ms, 'f', 1);
    }
    frame_ = std::move(frame);
    texture_dirty_ = true;
    update();
}

void ViewerGL::clear() {
    frame_.reset();
    texture_valid_ = false;
    texture_second_valid_ = false;
    nv12_valid_ = false;
    nv12_b_valid_ = false;
    vaapi_valid_ = false;
    vaapi_b_valid_ = false;
    grade_a_uploaded_ = nullptr;
    grade_b_uploaded_ = nullptr;
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

void ViewerGL::set_overlay(Overlay overlay, const bool on) {
    const unsigned bit = static_cast<unsigned>(overlay);
    const bool was_on = (overlay_flags_ & bit) != 0;
    if (was_on == on) return;
    if (on) overlay_flags_ |= bit;
    else overlay_flags_ &= ~bit;
    update();
}

bool ViewerGL::overlay_enabled(Overlay overlay) const {
    return (overlay_flags_ & static_cast<unsigned>(overlay)) != 0;
}

void ViewerGL::set_playing(const bool playing) {
    if (playing_ == playing) return;
    playing_ = playing;
    update();
}

void ViewerGL::initializeGL() {
    initializeOpenGLFunctions();

    const bool gles = context()->isOpenGLES();
    const QByteArray vertHdr = viewer_shader_header(gles, false);
    const QByteArray fragHdr = viewer_shader_header(gles, true);

    program_ = std::make_unique<QOpenGLShaderProgram>();
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertHdr + kVertexSrc);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragHdr + kFragSrc);
    program_->link();
    attr_pos_ = program_->attributeLocation("in_pos");
    attr_uv_ = program_->attributeLocation("in_uv");
    uni_mode_ = program_->uniformLocation("u_mode");
    uni_progress_ = program_->uniformLocation("u_progress");
    uni_aspect_ = program_->uniformLocation("u_aspect");

    program_nv12_ = std::make_unique<QOpenGLShaderProgram>();
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertHdr + kVertexSrc);
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragHdr + kFragNv12Src);
    program_nv12_->link();

    ::canvas::core::log::log_warning(
        "[viewer] nv12 programs: per-frame u_matrix/u_range from Nv12Frame spec "
        "(tags+probe), not hardcoded 709-limited");

    program_nv12_trans_ = std::make_unique<QOpenGLShaderProgram>();
    program_nv12_trans_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertHdr + kVertexSrc);
    program_nv12_trans_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragHdr + kFragNv12Trans);
    program_nv12_trans_->link();

    rgba_gl_ok_ = program_->isLinked();
    const bool nv12_ok = program_nv12_->isLinked();
    const bool trans_ok = program_nv12_trans_->isLinked();
    const auto gl_str = [](const GLubyte* s) -> const char* {
        return s ? reinterpret_cast<const char*>(s) : "(null)";
    };
    ::canvas::core::log::log_warning(
        "[viewer] gl context=%s programs rgba=%d nv12=%d trans=%d "
        "vendor=%s renderer=%s version=%s",
        context()->isOpenGLES() ? "es" : "desktop", rgba_gl_ok_ ? 1 : 0,
        nv12_ok ? 1 : 0, trans_ok ? 1 : 0, gl_str(glGetString(GL_VENDOR)),
        gl_str(glGetString(GL_RENDERER)), gl_str(glGetString(GL_VERSION)));
    if (!rgba_gl_ok_) {
        const std::string l = program_->log().toStdString();
        ::canvas::core::log::log_warning("[viewer] rgba program link: %s", l.c_str());
    }
    if (!nv12_ok) {
        const std::string l = program_nv12_->log().toStdString();
        ::canvas::core::log::log_warning("[viewer] nv12 program link: %s", l.c_str());
    }
    if (!trans_ok) {
        const std::string l = program_nv12_trans_->log().toStdString();
        ::canvas::core::log::log_warning("[viewer] nv12-trans program link: %s", l.c_str());
    }

    static const float kQuad[] = {
        -1.f, -1.f, 0.f, 1.f,
         1.f, -1.f, 1.f, 1.f,
        -1.f,  1.f, 0.f, 0.f,
         1.f,  1.f, 1.f, 0.f,
    };
    vbo_.create();
    vbo_.bind();
    vbo_.allocate(kQuad, sizeof(kQuad));

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

    texture_nv12_b_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_b_y_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_y_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_y_->setWrapMode(QOpenGLTexture::ClampToEdge);

    texture_nv12_b_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_b_uv_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);

    vaapi_importer_ = std::make_unique<VaapiViewerImporter>();
    const bool vaapi_ok = vaapi_importer_->available();
    canvas::gui::set_vaapi_viewer_import_available(vaapi_ok);
    ::canvas::core::log::log_warning(
        "[viewer] vaapi zero-copy import %s (need EGL+dma_buf ext; CPU NV12 path "
        "kept when unavailable)",
        vaapi_ok ? "available" : "unavailable");

    glGenTextures(1, &grade_neutral_tex_);
    glBindTexture(GL_TEXTURE_3D, grade_neutral_tex_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    const unsigned char neutral_lut[3] = {0, 0, 0};
    if (TexImage3DProc t3d = tex_image_3d_proc())
        t3d(GL_TEXTURE_3D, 0, GL_RGB8, 1, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, neutral_lut);
    glActiveTexture(GL_TEXTURE0);

    if (frame_) upload_frame();
}

void ViewerGL::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void ViewerGL::upload_frame() {
    if (!frame_ || !texture_) return;
    static auto up_agg_at = std::chrono::steady_clock::now();
    static int up_n = 0;
    static double up_nv12_ms = 0.0, up_rgba_ms = 0.0;
    static int up_nv12_cnt = 0, up_rgba_cnt = 0, up_cvt_cnt = 0, up_realloc_cnt = 0;
    const auto up_t0 = std::chrono::steady_clock::now();
    const auto up_mark = [&](const char* path) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - up_t0).count();
        ++up_n;
        if (path[0] == 'n') { up_nv12_ms += ms; ++up_nv12_cnt; }
        else { up_rgba_ms += ms; ++up_rgba_cnt; }
        const auto unow = std::chrono::steady_clock::now();
        if (up_n == 1 || unow - up_agg_at >= std::chrono::seconds(1)) {
            up_agg_at = unow;
            qDebug().nospace()
                << "[viewer] upload n=" << up_n
                << " nv12_ms=" << QString::number(
                       up_nv12_cnt ? up_nv12_ms / up_nv12_cnt : 0.0, 'f', 2)
                << " (" << up_nv12_cnt << "x)"
                << " rgba_ms=" << QString::number(
                       up_rgba_cnt ? up_rgba_ms / up_rgba_cnt : 0.0, 'f', 2)
                << " (" << up_rgba_cnt << "x)"
                << " nv12->rgba_cvt=" << up_cvt_cnt
                << " texture_realloc=" << up_realloc_cnt;
            up_n = 0;
            up_nv12_ms = up_rgba_ms = 0.0;
            up_nv12_cnt = up_rgba_cnt = 0;
            up_cvt_cnt = 0;
            up_realloc_cnt = 0;
        }
    };

    auto upload = [this](std::unique_ptr<QOpenGLTexture>& tex, const canvas::core::VideoFramePtr& f,
                     int& tw, int& th, bool& valid) {
        if (!f || f->rgba.empty()) return;
        int w = f->width;
        int h = f->height;
        const uint8_t* data = f->rgba.data();
        std::size_t stride = f->stride;
        QImage upscaled;

        const int vw = std::max(1, width());
        const int vh = std::max(1, height());
        if (w > 0 && h > 0 && (vw > w || vh > h) &&
            vw > 2 && vh > 2 && static_cast<std::size_t>(w) * h * 4ULL <= f->rgba.size()) {
            const double scale = std::min(static_cast<double>(vw) / w,
                                          static_cast<double>(vh) / h);
            int dw = std::max(1, static_cast<int>(std::llround(w * scale)));
            int dh = std::max(1, static_cast<int>(std::llround(h * scale)));
            if (dw != w || dh != h) {
                static int upscale_log_ = 0;
                if ((upscale_log_++ % 12) == 0)
qDebug() << "[viewer] UPSCALE"
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
                    upscaled = std::move(dst);
                    w = dw;
                    h = dh;
                    data = upscaled.constBits();
                    stride = static_cast<std::size_t>(dw) * 4;
                }
            }
        }

        const bool realloc = tw != w || th != h;
        if (realloc) ++up_realloc_cnt;
        static int64_t tex_log_ = 0;
        if ((tex_log_++ % 16) == 0)
            qDebug() << "[viewer] rgba_upload"
                       << "tex=" << w << "x" << h
                       << "src_frame=" << f->width << "x" << f->height
                       << "upscaled=" << (w != f->width || h != f->height ? "yes" : "no")
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
        if (realloc) {
            tex = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            tex->create();
            tex->setMinificationFilter(QOpenGLTexture::Linear);
            tex->setMagnificationFilter(QOpenGLTexture::Linear);
            tex->setWrapMode(QOpenGLTexture::ClampToEdge);
            glBindTexture(GL_TEXTURE_2D, tex->textureId());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            tw = w;
            th = h;
        }
        tex->setData(0, 0, 0, w, h, 1, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, data);
        valid = true;
    };

    auto upload_grades = [this](const canvas::core::RenderFrame* rf) {
        {   const canvas::core::grade_graph::GradeLut3D* lut = rf->grade.get();
            if (lut && lut->valid() && lut != grade_a_uploaded_) {
                const int n = lut->size;
                if (grade_tex_a_ &&
                    grade_tex_a_->isStorageAllocated() &&
                    grade_tex_a_->width() != n) {
                    grade_tex_a_.reset();
                }
                const bool fresh = !grade_tex_a_ || !grade_tex_a_->isStorageAllocated();
                if (!grade_tex_a_) {
                    grade_tex_a_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target3D);
                    grade_tex_a_->setMinificationFilter(QOpenGLTexture::Linear);
                    grade_tex_a_->setMagnificationFilter(QOpenGLTexture::Linear);
                    grade_tex_a_->setWrapMode(QOpenGLTexture::ClampToEdge);
                }
                if (fresh) {
                    grade_tex_a_->setSize(n, n, n);
                    grade_tex_a_->setFormat(QOpenGLTexture::RGB32F);
                    grade_tex_a_->allocateStorage();
                }
                const auto up0 = std::chrono::steady_clock::now();
                grade_tex_a_->setData(0, 0, 0, n, n, n, QOpenGLTexture::RGB,
                                      QOpenGLTexture::Float32, lut->data.data());
                const double up_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - up0)
                                         .count();
                const auto digest = canvas::core::grade_graph::grade_lut_digest(*lut);
                qDebug().nospace()
                    << "[grade] viewer LUT-A upload seq=" << lut->change_seq
                    << " t=" << canvas::core::log::epoch_ms()
                    << " size=" << n << " up_ms=" << QString::number(up_ms, 'f', 3)
                    << " frame=" << (rf->a ? rf->a->frame_number : -1)
                    << " realloc=" << (fresh ? 1 : 0)
                    << " tex=" << static_cast<const void*>(grade_tex_a_.get())
                    << " hash=" << QString::number(digest.hash, 16)
                    << " mid=(" << QString::number(digest.mid[0], 'f', 3) << ","
                    << QString::number(digest.mid[1], 'f', 3) << ","
                    << QString::number(digest.mid[2], 'f', 3) << ")"
                    << " black=(" << QString::number(digest.black[0], 'f', 3) << ","
                    << QString::number(digest.black[1], 'f', 3) << ","
                    << QString::number(digest.black[2], 'f', 3) << ")"
                    << " skin=(" << QString::number(digest.skin[0], 'f', 3) << ","
                    << QString::number(digest.skin[1], 'f', 3) << ","
                    << QString::number(digest.skin[2], 'f', 3) << ")"
                    << " maxdev=" << QString::number(digest.max_dev, 'f', 3);
                CANVAS_COLOR_LOG(
                    "[viewer] upload A seq=%llu size=%d hash=%016llx "
                    "mid=(%.3f,%.3f,%.3f) skin=(%.3f,%.3f,%.3f) maxdev=%.3f",
                    static_cast<unsigned long long>(lut->change_seq), n,
                    static_cast<unsigned long long>(digest.hash), digest.mid[0],
                    digest.mid[1], digest.mid[2], digest.skin[0], digest.skin[1],
                    digest.skin[2], digest.max_dev);
                grade_a_uploaded_ = lut;
            } else if (!lut) {
                grade_tex_a_.reset();
                grade_a_uploaded_ = nullptr;
            }
        }
        {   const canvas::core::grade_graph::GradeLut3D* lut = rf->grade_b.get();
            if (lut && lut->valid() && lut != grade_b_uploaded_) {
                const int n = lut->size;
                if (grade_tex_b_ &&
                    grade_tex_b_->isStorageAllocated() &&
                    grade_tex_b_->width() != n) {
                    grade_tex_b_.reset();
                }
                const bool fresh = !grade_tex_b_ || !grade_tex_b_->isStorageAllocated();
                if (!grade_tex_b_) {
                    grade_tex_b_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target3D);
                    grade_tex_b_->setMinificationFilter(QOpenGLTexture::Linear);
                    grade_tex_b_->setMagnificationFilter(QOpenGLTexture::Linear);
                    grade_tex_b_->setWrapMode(QOpenGLTexture::ClampToEdge);
                }
                if (fresh) {
                    grade_tex_b_->setSize(n, n, n);
                    grade_tex_b_->setFormat(QOpenGLTexture::RGB32F);
                    grade_tex_b_->allocateStorage();
                }
                const auto up0 = std::chrono::steady_clock::now();
                grade_tex_b_->setData(0, 0, 0, n, n, n, QOpenGLTexture::RGB,
                                      QOpenGLTexture::Float32, lut->data.data());
                const double up_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - up0)
                                         .count();
                const auto digest = canvas::core::grade_graph::grade_lut_digest(*lut);
                qDebug().nospace()
                    << "[grade] viewer LUT-B upload seq=" << lut->change_seq
                    << " t=" << canvas::core::log::epoch_ms()
                    << " size=" << n << " up_ms=" << QString::number(up_ms, 'f', 3)
                    << " frame=" << (rf->b ? rf->b->frame_number : -1)
                    << " realloc=" << (fresh ? 1 : 0)
                    << " tex=" << static_cast<const void*>(grade_tex_b_.get())
                    << " hash=" << QString::number(digest.hash, 16)
                    << " mid=(" << QString::number(digest.mid[0], 'f', 3) << ","
                    << QString::number(digest.mid[1], 'f', 3) << ","
                    << QString::number(digest.mid[2], 'f', 3) << ")"
                    << " skin=(" << QString::number(digest.skin[0], 'f', 3) << ","
                    << QString::number(digest.skin[1], 'f', 3) << ","
                    << QString::number(digest.skin[2], 'f', 3) << ")"
                    << " maxdev=" << QString::number(digest.max_dev, 'f', 3);
                grade_b_uploaded_ = lut;
            } else if (!lut) {
                grade_tex_b_.reset();
                grade_b_uploaded_ = nullptr;
            }
        }
    };

    if (frame_->nv12 && frame_->nv12->has_gpu()) {
        vaapi_valid_ = false;
        vaapi_b_valid_ = false;
        const canvas::core::Nv12Frame* nv = frame_->nv12.get();
        if (nv->gpu && nv->gpu->valid()) {
            if (!vaapi_importer_) vaapi_importer_ = std::make_unique<VaapiViewerImporter>();
            GLuint yt = 0, uv = 0;
            if (vaapi_importer_->import(*nv->gpu, &yt, &uv)) {
                vaapi_tex_y_ = yt;
                vaapi_tex_uv_ = uv;
                vaapi_valid_ = true;
                if (nv->width > 0) {
                    tex_w_ = nv->width;
                    tex_h_ = nv->height;
                }
            } else {
                ::canvas::core::log::log_warning(
                    "[viewer] vaapi A import failed frame=%lld",
                    static_cast<long long>(nv->frame_number));
            }
        }
        if (frame_->b_nv12 && frame_->b_nv12->gpu && frame_->b_nv12->gpu->valid()) {
            if (!vaapi_importer_b_) vaapi_importer_b_ = std::make_unique<VaapiViewerImporter>();
            GLuint by = 0, buv = 0;
            if (vaapi_importer_b_->import(*frame_->b_nv12->gpu, &by, &buv)) {
                vaapi_tex_b_y_ = by;
                vaapi_tex_b_uv_ = buv;
                vaapi_b_valid_ = true;
            } else {
                ::canvas::core::log::log_warning(
                    "[viewer] vaapi B import failed frame=%lld",
                    static_cast<long long>(frame_->b_nv12->frame_number));
            }
        }
        nv12_b_valid_ = vaapi_b_valid_;
        if (vaapi_valid_) {
            upload_grades(frame_.get());
            texture_valid_ = false;
            texture_second_valid_ = false;
            nv12_valid_ = true;
            texture_dirty_ = false;
            up_mark("n");
            return;
        }
        nv12_valid_ = false;
        texture_valid_ = false;
    }

    if (frame_->nv12 && !frame_->nv12->y.empty()) {
        const canvas::core::Nv12Frame* n = frame_->nv12.get();
        const int w = n->width;
        const int h = n->height;

        if (w > 0 && h > 0 && (w < width() || h < height())) {
            static bool yuv2rgb_logged_ = false;
            if (!yuv2rgb_logged_) {
                yuv2rgb_logged_ = true;
                ::canvas::core::log::log_warning(
                    "[viewer] cpu NV12->RGBA fallback: yuv_to_rgb uses per-frame "
                    "Nv12Frame matrix/range (matrix=%s range=%s)",
                    canvas::core::gpu::color_matrix_name(n->matrix),
                    canvas::core::gpu::color_range_name(n->range));
            }
            static int64_t nv12cvt_log_ = 0;
            if ((nv12cvt_log_++ % 16) == 0)
                qDebug() << "[viewer] NV12->RGBA"
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
                    const auto rgb = canvas::core::gpu::yuv_to_rgb(yrow[x], cb, cr,
                                                                   n->range, n->matrix);
                    prow[x * 4 + 0] = rgb.r;
                    prow[x * 4 + 1] = rgb.g;
                    prow[x * 4 + 2] = rgb.b;
                    prow[x * 4 + 3] = 255;
                }
            }
            auto rf = std::make_shared<canvas::core::RenderFrame>();
            canvas::core::VideoFramePtr carry = std::move(rgba);

            rf->a = std::move(carry);
            rf->grade = frame_->grade;
            rf->grade_b = frame_->grade_b;
            rf->b = frame_->b;
            rf->mode = frame_->mode;
            rf->progress = frame_->progress;
            rf->fade_from_black = frame_->fade_from_black;
            rf->fade_to_black = frame_->fade_to_black;
            frame_ = std::move(rf);
            nv12_valid_ = false;
            upload(texture_, frame_->a, tex_w_, tex_h_, texture_valid_);
            texture_second_valid_ = false;
            if (frame_->b && frame_->b->rgba.size() >= frame_->b->stride * frame_->b->height) {
                int bw = 0;
                int bh = 0;
                upload(texture_b_, frame_->b, bw, bh, texture_second_valid_);
            }
            upload_grades(frame_.get());
            ++up_cvt_cnt;
            texture_dirty_ = false;
            up_mark("r");
            return;
        }

        const bool y_realloc = tex_w_ != w || tex_h_ != h;
        if (y_realloc) {
            ++up_realloc_cnt;
            texture_nv12_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            texture_nv12_y_->create();
            texture_nv12_y_->setMinificationFilter(QOpenGLTexture::Linear);
            texture_nv12_y_->setMagnificationFilter(QOpenGLTexture::Linear);
            texture_nv12_y_->setWrapMode(QOpenGLTexture::ClampToEdge);
            glBindTexture(GL_TEXTURE_2D, texture_nv12_y_->textureId());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
            texture_nv12_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            texture_nv12_uv_->create();
            texture_nv12_uv_->setMinificationFilter(QOpenGLTexture::Linear);
            texture_nv12_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
            texture_nv12_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);
            glBindTexture(GL_TEXTURE_2D, texture_nv12_uv_->textureId());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, w / 2, h / 2, 0, GL_RG, GL_UNSIGNED_BYTE,
                        nullptr);
            tex_w_ = w;
            tex_h_ = h;
        }
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
        nv12_b_valid_ = false;
        if (frame_->b_nv12 && !frame_->b_nv12->y.empty()) {
            const canvas::core::Nv12Frame* bn = frame_->b_nv12.get();
            const int bw = bn->width;
            const int bh = bn->height;
            const bool b_realloc = tex_bw_ != bw || tex_bh_ != bh;
            if (b_realloc) {
                ++up_realloc_cnt;
                texture_nv12_b_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
                texture_nv12_b_y_->create();
                texture_nv12_b_y_->setMinificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_y_->setMagnificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_y_->setWrapMode(QOpenGLTexture::ClampToEdge);
                glBindTexture(GL_TEXTURE_2D, texture_nv12_b_y_->textureId());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, bw, bh, 0, GL_RED, GL_UNSIGNED_BYTE,
                            nullptr);
                texture_nv12_b_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
                texture_nv12_b_uv_->create();
                texture_nv12_b_uv_->setMinificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);
                glBindTexture(GL_TEXTURE_2D, texture_nv12_b_uv_->textureId());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, bw / 2, bh / 2, 0, GL_RG,
                            GL_UNSIGNED_BYTE, nullptr);
                tex_bw_ = bw;
                tex_bh_ = bh;
            }
            const int b_y_pitch = static_cast<int>(bn->y_pitch);
            const int b_uv_pitch = static_cast<int>(bn->uv_pitch);
            if (b_y_pitch == bw && b_uv_pitch == bw) {
                texture_nv12_b_y_->setData(0, 0, 0, bw, bh, 1, QOpenGLTexture::Red,
                                           QOpenGLTexture::UInt8, bn->y.data());
                texture_nv12_b_uv_->setData(0, 0, 0, bw / 2, bh / 2, 1, QOpenGLTexture::RG,
                                            QOpenGLTexture::UInt8, bn->uv.data());
            } else {
                for (int row = 0; row < bh; ++row)
                    texture_nv12_b_y_->setData(0, 0, row, bw, 1, 1, QOpenGLTexture::Red,
                                               QOpenGLTexture::UInt8,
                                               bn->y.data() + row * b_y_pitch);
                for (int row = 0; row < bh / 2; ++row)
                    texture_nv12_b_uv_->setData(0, 0, row, bw / 2, 1, 1, QOpenGLTexture::RG,
                                                QOpenGLTexture::UInt8,
                                                bn->uv.data() + row * b_uv_pitch);
            }
            nv12_b_valid_ = true;
        }
        upload_grades(frame_.get());
        texture_valid_ = false;
        texture_second_valid_ = false;
        nv12_valid_ = true;
        texture_dirty_ = false;
        up_mark("n");
        static int64_t nv12up_log_ = 0;
        if ((nv12up_log_++ % 16) == 0)
            qDebug() << "[viewer] nv12_upload"
                       << "tex=" << w << "x" << h
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
        return;
    }

    nv12_valid_ = false;
    if (!frame_->a || frame_->a->rgba.empty()) {
        texture_dirty_ = false;
        up_mark("r");
        return;
    }

    upload(texture_, frame_->a, tex_w_, tex_h_, texture_valid_);

    texture_second_valid_ = false;
    if (frame_->b && frame_->b->rgba.size() >= frame_->b->stride * frame_->b->height) {
        int bw = 0;
        int bh = 0;
        upload(texture_b_, frame_->b, bw, bh, texture_second_valid_);
    }

    upload_grades(frame_.get());

    texture_dirty_ = false;
    up_mark("r");
}

void ViewerGL::paintGL() {
    const qreal dpr = devicePixelRatioF();
    glViewport(0, 0, std::max(1, static_cast<int>(std::lround(width() * dpr))),
               std::max(1, static_cast<int>(std::lround(height() * dpr))));

    while (glGetError() != GL_NO_ERROR) {}
    GLenum first_err = GL_NO_ERROR;
    int err_stage = -1;
    const auto note_err = [&](int s) {
        if (first_err != GL_NO_ERROR) return;
        const GLenum e = glGetError();
        if (e != GL_NO_ERROR) {
            first_err = e;
            err_stage = s;
        }
    };

    const QColor bg = viewer_background_color();
    glClearColor(bg.redF(), bg.greenF(), bg.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!rgba_gl_ok_) {
        const canvas::core::VideoFrame* a = frame_ ? frame_->a.get() : nullptr;
        if (a && !a->rgba.empty() && a->width > 0 && a->height > 0 &&
            a->stride > 0 && a->rgba.size() >=
                                 static_cast<std::size_t>(a->stride) * a->height) {
            tex_w_ = a->width;
            tex_h_ = a->height;
            const float vw = static_cast<float>(width());
            const float vh = static_cast<float>(height());
            if (vw >= 8.0f && vh >= 8.0f) {
                const float fa = static_cast<float>(tex_w_) / static_cast<float>(tex_h_);
                const float va = vw / vh;
                const float qw = (scale_mode_ == ScaleMode::Fill)
                                     ? std::max(fa / va, 1.0f)
                                     : std::min(fa / va, 1.0f);
                const float qh = (scale_mode_ == ScaleMode::Fill)
                                     ? std::max(va / fa, 1.0f)
                                     : std::min(va / fa, 1.0f);
                const QRectF target((vw - qw * vw) / 2.0, (vh - qh * vh) / 2.0,
                                    qw * vw, qh * vh);
                QPainter p(this);
                if (viewer_background_ == ViewerBackground::Checkerboard)
                    paint_checkerboard(p, rect());
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                const QImage img(a->rgba.data(), a->width, a->height, a->stride,
                                 QImage::Format_RGBA8888);
                p.drawImage(target, img);
                p.end();
                draw_viewer_overlays();
                return;
            }
        }
    }

    if (debug_enabled() && texture_dirty_)
        qDebug() << "viewer: paintGL dirty, uploading frame"
                 << (frame_ && frame_->nv12 ? frame_->nv12->frame_number
                    : (frame_ && frame_->a ? frame_->a->frame_number : -1));

    const bool has_frame_texture = texture_valid_ || nv12_valid_;
    if (!has_frame_texture) {
        static int64_t blank_log_ = 0;
        if ((blank_log_++ % 30) == 0)
            ::canvas::core::log::log_warning(
                "[viewer] paint uid=%llu BLANK tex=%dx%d widget=%dx%d dirty=%d "
                "frame_rgba=%d frame_nv12=%d rgba_ok=%d",
                static_cast<unsigned long long>(viewer_uid_), tex_w_, tex_h_,
                std::max(1, width()), std::max(1, height()),
                static_cast<int>(texture_dirty_),
                frame_ && frame_->a && !frame_->a->rgba.empty() ? 1 : 0,
                frame_ && frame_->nv12 ? 1 : 0, static_cast<int>(rgba_gl_ok_));
        if (texture_dirty_) upload_frame();
        if (!(texture_valid_ || nv12_valid_)) {
            draw_blank();
            return;
        }
    } else if (texture_dirty_) {
        upload_frame();
    }

    const float vw = static_cast<float>(width());
    const float vh = static_cast<float>(height());
    const float aspect = tex_h_ > 0 ? static_cast<float>(tex_w_) / tex_h_ : 1.0f;
    const float va = vw / vh;

    const float qw = (scale_mode_ == ScaleMode::Fill)
                         ? std::max(aspect / va, 1.0f)
                         : std::min(aspect / va, 1.0f);
    const float qh = (scale_mode_ == ScaleMode::Fill)
                         ? std::max(va / aspect, 1.0f)
                         : std::min(va / aspect, 1.0f);
    {
        static int64_t paint_log_ = 0;
        static auto log_t0 = std::chrono::steady_clock::now();
        static double sum_ms = 0.0;
        static int64_t sum_n = 0;
        const auto p0 = std::chrono::steady_clock::now();
        if ((paint_log_++ % 30) == 0)
            ::canvas::core::log::log_warning(
                "[viewer] paint uid=%llu tex=%dx%d widget=%dx%d tex_valid=%d "
                "nv12_valid=%d vaapi_valid=%d dirty=%d rgba_ok=%d",
                static_cast<unsigned long long>(viewer_uid_), tex_w_, tex_h_,
                static_cast<int>(vw), static_cast<int>(vh), static_cast<int>(texture_valid_),
                static_cast<int>(nv12_valid_), static_cast<int>(vaapi_valid_),
                static_cast<int>(texture_dirty_), static_cast<int>(rgba_gl_ok_));
        const auto p1 = std::chrono::steady_clock::now();
        const double paint_ms =
            std::chrono::duration<double, std::milli>(p1 - p0).count();
        sum_ms += paint_ms;
        ++sum_n;
        const double since_s = std::chrono::duration<double>(p1 - log_t0).count();
        if (since_s >= 1.0) {
            qDebug().nospace()
                << "[viewer] paint avg_ms="
                << QString::number(sum_ms / static_cast<double>(sum_n), 'f', 2)
                << " last_ms=" << QString::number(paint_ms, 'f', 2)
                << " n=" << sum_n
                << " starved=" << (texture_dirty_ ? "upload_pending" : "no_new_frame");
            log_t0 = p1;
            sum_ms = 0.0;
            sum_n = 0;
        }
    }

    const bool single_fade = frame_ && (frame_->fade_from_black || frame_->fade_to_black);

    const bool nv12_cur = nv12_valid_ && frame_ && frame_->nv12;
    const bool nv12_blend =
        nv12_cur &&
        (single_fade || frame_->mode != canvas::core::TransitionRenderMode::None);

    {
        const bool grade_present = frame_ && frame_->grade && frame_->grade->valid();
        const bool grade_bound = grade_present && grade_tex_a_ &&
                                 grade_a_uploaded_ == frame_->grade.get();
        static const void* last_lut = nullptr;
        if (grade_present) {
            const void* cur = frame_->grade.get();
            if (cur != last_lut) {
                last_lut = cur;
                const auto digest = frame_->grade
                    ? canvas::core::grade_graph::grade_lut_digest(*frame_->grade)
                    : canvas::core::grade_graph::GradeLutDigest{};
                qDebug().nospace()
                    << "[grade] viewer draw seq="
                    << (frame_->grade ? frame_->grade->change_seq : 0)
                    << " t=" << canvas::core::log::epoch_ms()
                    << " path="
                    << (nv12_blend ? "nv12-trans"
                                   : (nv12_cur ? "nv12" : "rgba"))
                    << " present=" << (grade_present ? 1 : 0)
                    << " bound=" << (grade_bound ? 1 : 0)
                    << " drop=" << (grade_present && !grade_bound ? 1 : 0)
                    << " size=" << (grade_present ? frame_->grade->size : 0)
                    << " tex=" << (grade_tex_a_ ? static_cast<const void*>(grade_tex_a_.get())
                                                : nullptr)
                    << " hash=" << QString::number(digest.hash, 16)
                    << " mid=(" << QString::number(digest.mid[0], 'f', 3) << ","
                    << QString::number(digest.mid[1], 'f', 3) << ","
                    << QString::number(digest.mid[2], 'f', 3) << ")"
                    << " skin=(" << QString::number(digest.skin[0], 'f', 3) << ","
                    << QString::number(digest.skin[1], 'f', 3) << ","
                    << QString::number(digest.skin[2], 'f', 3) << ")"
                    << " maxdev=" << QString::number(digest.max_dev, 'f', 3);
            }
        } else {
            last_lut = nullptr;
        }
    }

    if (nv12_blend) {
        program_nv12_trans_->bind();
        vao_.bind();
        bind_nv12_a(0, 1);
        program_nv12_trans_->setUniformValue("u_tex_y", 0);
        program_nv12_trans_->setUniformValue("u_tex_uv", 1);
        const bool have_b = nv12_b_valid_ && frame_->b_nv12;
        if (have_b) {
            bind_nv12_b(2, 3);
        } else {
            bind_nv12_a(2, 3);
        }
        program_nv12_trans_->setUniformValue("u_tex_b_y", 2);
        program_nv12_trans_->setUniformValue("u_tex_b_uv", 3);
        const auto spec_int2 = [](const canvas::core::Nv12Frame* n) {
            return std::pair<int, int>{static_cast<int>(n->matrix),
                                       static_cast<int>(n->range)};
        };
        const auto spec_a = spec_int2(frame_->nv12.get());
        program_nv12_trans_->setUniformValue("u_matrix_a", spec_a.first);
        program_nv12_trans_->setUniformValue("u_range_a", spec_a.second);
        const auto spec_b = spec_int2(have_b ? frame_->b_nv12.get() : frame_->nv12.get());
        program_nv12_trans_->setUniformValue("u_matrix_b", spec_b.first);
        program_nv12_trans_->setUniformValue("u_range_b", spec_b.second);
        const bool grade_a_attached = frame_->grade && frame_->grade->valid() &&
                                      grade_tex_a_ && grade_a_uploaded_ == frame_->grade.get();
        const bool grade_b_attached = have_b && frame_->grade_b &&
                                      frame_->grade_b->valid() && grade_tex_b_ &&
                                      grade_b_uploaded_ == frame_->grade_b.get();
        bind_grade_lut(4, grade_a_attached ? grade_tex_a_.get() : nullptr);
        program_nv12_trans_->setUniformValue("u_grade_a", 4);
        program_nv12_trans_->setUniformValue("u_grade_a_size",
                                             grade_a_attached ? frame_->grade->size : 0);
        bind_grade_lut(5, grade_b_attached ? grade_tex_b_.get() : nullptr);
        program_nv12_trans_->setUniformValue("u_grade_b", 5);
        program_nv12_trans_->setUniformValue("u_grade_b_size",
                                             grade_b_attached ? frame_->grade_b->size : 0);
        if (single_fade) {
            const int fade_mode = frame_->fade_from_black ? 9 : 3;
            program_nv12_trans_->setUniformValue("u_mode", fade_mode);
            program_nv12_trans_->setUniformValue("u_progress", frame_->progress);
        } else {
            program_nv12_trans_->setUniformValue("u_mode",
                                                 static_cast<int>(frame_->mode));
            program_nv12_trans_->setUniformValue("u_progress", frame_->progress);
        }
        program_nv12_trans_->setUniformValue("u_aspect", aspect);
    } else if (nv12_cur) {
        program_nv12_->bind();
        vao_.bind();
        bind_nv12_a(0, 1);
        program_nv12_->setUniformValue("u_tex_y", 0);
        program_nv12_->setUniformValue("u_tex_uv", 1);
        program_nv12_->setUniformValue("u_matrix", static_cast<int>(frame_->nv12->matrix));
        program_nv12_->setUniformValue("u_range", static_cast<int>(frame_->nv12->range));
        const bool grade_a_attached = frame_->grade && frame_->grade->valid() &&
                                      grade_tex_a_ && grade_a_uploaded_ == frame_->grade.get();
        const int grade_state = grade_a_attached ? 1 : 0;
        if (!last_spec_set_ || frame_->nv12->matrix != last_spec_matrix_ ||
            frame_->nv12->range != last_spec_range_ || grade_state != last_grade_attached_) {
            last_spec_set_ = true;
            last_spec_matrix_ = frame_->nv12->matrix;
            last_spec_range_ = frame_->nv12->range;
            last_grade_attached_ = grade_state;
            CANVAS_COLOR_LOG(
                "[viewer] nv12 draw spec matrix=%s range=%s grade_attached=%d",
                canvas::core::gpu::color_matrix_name(frame_->nv12->matrix),
                canvas::core::gpu::color_range_name(frame_->nv12->range), grade_state);
        }
        bind_grade_lut(4, grade_a_attached ? grade_tex_a_.get() : nullptr);
        program_nv12_->setUniformValue("u_grade", 4);
        program_nv12_->setUniformValue("u_grade_size",
                                       grade_a_attached ? frame_->grade->size : 0);
    } else {
        program_->bind();
        note_err(0);
        vao_.bind();
        note_err(1);
        texture_->bind(0);
        note_err(2);
        program_->setUniformValue("u_tex", 0);

        if (single_fade) {
            texture_->bind(1);
            program_->setUniformValue("u_tex_b", 1);
            const int fade_mode = frame_->fade_from_black ? 9 : 3;
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
        const bool grade_a_attached = frame_ && frame_->grade && frame_->grade->valid() &&
                                      grade_tex_a_ && grade_a_uploaded_ == frame_->grade.get();
        const bool grade_b_attached = frame_ && frame_->grade_b && frame_->grade_b->valid() &&
                                      grade_tex_b_ && grade_b_uploaded_ == frame_->grade_b.get();
        bind_grade_lut(4, grade_a_attached ? grade_tex_a_.get() : nullptr);
        program_->setUniformValue("u_grade", 4);
        program_->setUniformValue("u_grade_size",
                                  grade_a_attached ? frame_->grade->size : 0);
        bind_grade_lut(5, grade_b_attached ? grade_tex_b_.get() : nullptr);
        program_->setUniformValue("u_grade_b", 5);
        program_->setUniformValue("u_grade_b_size",
                                  grade_b_attached ? frame_->grade_b->size : 0);
        program_->setUniformValue("u_aspect", aspect);
        note_err(3);
    }

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
            const double bx = corners[k][0] + cx - px;
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
            s[i] *= qw;
            s[i + 1] *= qh;
        }
    }
    vbo_.bind();
    vbo_.write(0, s.data(), sizeof(float) * s.size());
    note_err(4);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    note_err(5);

    static int64_t px_probe_ = 0;
    const bool px_on_err = first_err != GL_NO_ERROR;
    if (px_on_err || (px_probe_++ % 60) == 0) {
        const auto err_name = [](GLenum e) -> const char* {
            switch (e) {
                case GL_NO_ERROR: return "none";
                case GL_INVALID_ENUM: return "invalid-enum";
                case GL_INVALID_VALUE: return "invalid-value";
                case GL_INVALID_OPERATION: return "invalid-op";
                case GL_INVALID_FRAMEBUFFER_OPERATION: return "invalid-fbo";
                case GL_OUT_OF_MEMORY: return "oom";
                default: return "other";
            }
        };
        uint8_t fbpx[4] = {0, 0, 0, 0};
        int fb_nz = 0;
        int fb_n = 0;
        long long fb_sum = 0;
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);
        if (vp[2] > 0 && vp[3] > 0) {
            const double q = vp[2] / 4.0;
            const double r = vp[3] / 4.0;
            for (int c = 1; c <= 3; ++c) {
                for (int i = 1; i <= 3; ++i) {
                    uint8_t px[4] = {0, 0, 0, 0};
                    const int x = c == 2 ? vp[2] / 2 : static_cast<int>(q * c);
                    const int y = i == 2 ? vp[3] / 2 : static_cast<int>(r * i);
                    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    const int lum = px[0] + px[1] + px[2];
                    fb_sum += lum;
                    ++fb_n;
                    if (lum > 12) ++fb_nz;
                    if (c == 2 && i == 2)
                        for (int k = 0; k < 4; ++k) fbpx[k] = px[k];
                }
            }
        }
        const int fb_avg = fb_n ? static_cast<int>(fb_sum / (3LL * fb_n)) : -1;
        int s00[4] = {0, 0, 0, 0};
        int scc[4] = {0, 0, 0, 0};
        int src_avg = -1;
        if (frame_ && frame_->a && !frame_->a->rgba.empty() &&
            frame_->a->stride >= static_cast<std::size_t>(frame_->a->width) * 4) {
            const uint8_t* d = frame_->a->rgba.data();
            const int w = frame_->a->width;
            const int h = frame_->a->height;
            const std::size_t srow = frame_->a->stride;
            const auto px = [d, srow](int x, int y, int out[4]) {
                const std::size_t off =
                    static_cast<std::size_t>(y) * srow + static_cast<std::size_t>(x) * 4;
                for (int c = 0; c < 4; ++c) out[c] = d[off + c];
            };
            px(0, 0, s00);
            px(w / 2, h / 2, scc);
            long long sum = 0;
            int n = 0;
            for (int y = 0; y < h; y += 64) {
                const std::size_t row = static_cast<std::size_t>(y) * srow;
                for (int x = 0; x < w; x += 64) {
                    const std::size_t o = row + static_cast<std::size_t>(x) * 4;
                    sum += d[o] + d[o + 1] + d[o + 2];
                    ++n;
                }
            }
            src_avg = n ? static_cast<int>(sum / (3LL * n)) : 0;
        }
        ::canvas::core::log::log_warning(
            "[viewer] pixels uid=%llu errstage=%d err=%s tex=%dx%d win=%dx%d fb_avg=%d "
            "fb_nz=%d/%d fb=(%d,%d,%d,%d) src00=(%d,%d,%d,%d) "
            "srcc=(%d,%d,%d,%d) src_avg=%d",
            static_cast<unsigned long long>(viewer_uid_), err_stage, err_name(first_err),
            tex_w_, tex_h_, width(), height(), fb_avg, fb_nz, fb_n, fbpx[0], fbpx[1],
            fbpx[2], fbpx[3], s00[0], s00[1], s00[2], s00[3], scc[0], scc[1], scc[2],
            scc[3], src_avg);
        if (px_on_err) {
            GLint prog = 0, vao = 0, arb = 0, eab = 0, active_tex = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arb);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eab);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
            GLint vp2[4] = {0, 0, 0, 0};
            glGetIntegerv(GL_VIEWPORT, vp2);
            ::canvas::core::log::log_warning(
                "[viewer] gldump uid=%llu stage=%d prog=%d vao=%d arb=%d eab=%d "
                "act_tex=%d vp=%dx%d+%d+%d",
                static_cast<unsigned long long>(viewer_uid_), err_stage, prog, vao, arb,
                eab, active_tex, vp2[2], vp2[3], vp2[0], vp2[1]);
            for (int u = 0; u < 6; ++u) {
                glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + u));
                GLint t2d = 0, t3d = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &t2d);
                glGetIntegerv(GL_TEXTURE_BINDING_3D, &t3d);
                ::canvas::core::log::log_warning(
                    "[viewer] gldump uid=%llu unit%d 2d=%d 3d=%d",
                    static_cast<unsigned long long>(viewer_uid_), u, t2d, t3d);
            }
            glActiveTexture(GL_TEXTURE0);
            if (grade_neutral_tex_) {
                glBindTexture(GL_TEXTURE_3D, grade_neutral_tex_);
                GLint w3 = 0, h3 = 0, d3 = 0, fmt3 = 0, base = 0, minf = 0, magf = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_WIDTH, &w3);
                glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_HEIGHT, &h3);
                glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_DEPTH, &d3);
                glGetTexLevelParameteriv(GL_TEXTURE_3D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt3);
                glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, &base);
                glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, &minf);
                glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, &magf);
                ::canvas::core::log::log_warning(
                    "[viewer] gldump uid=%llu neutral3d id=%d lvl0=%dx%dx%d fmt=0x%x "
                    "base=%d min=0x%x mag=0x%x istex=%d",
                    static_cast<unsigned long long>(viewer_uid_), grade_neutral_tex_, w3,
                    h3, d3, fmt3, base, minf, magf,
                    glIsTexture(grade_neutral_tex_) ? 1 : 0);
            } else {
                ::canvas::core::log::log_warning(
                    "[viewer] gldump uid=%llu neutral3d id=0 (NOT CREATED)",
                    static_cast<unsigned long long>(viewer_uid_));
            }
            if (texture_ && texture_->textureId()) {
                glBindTexture(GL_TEXTURE_2D, texture_->textureId());
                GLint g = glGetError();
                GLint fmt2 = 0, w2 = 0, h2 = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &fmt2);
                if (glGetError() == GL_NO_ERROR) {
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w2);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h2);
                    ::canvas::core::log::log_warning(
                        "[viewer] gldump uid=%llu video2d id=%d lvl0=%dx%d fmt=0x%x",
                        static_cast<unsigned long long>(viewer_uid_),
                        texture_->textureId(), w2, h2, fmt2);
                } else {
                    ::canvas::core::log::log_warning(
                        "[viewer] gldump uid=%llu video2d id=%d glerr_after_bind=%d",
                        static_cast<unsigned long long>(viewer_uid_),
                        texture_->textureId(), static_cast<int>(g));
                }
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }
    }

    if (nv12_blend) {
        if (grade_a_uploaded_ && frame_->grade) grade_tex_a_->release();
        if (grade_b_uploaded_ && frame_->grade_b) grade_tex_b_->release();
        if (nv12_b_valid_ && frame_->b_nv12) {
            texture_nv12_b_uv_->release();
            texture_nv12_b_y_->release();
        } else {
            texture_nv12_uv_->release();
            texture_nv12_y_->release();
        }
        texture_nv12_uv_->release();
        texture_nv12_y_->release();
        program_nv12_trans_->release();
    } else if (nv12_cur) {
        if (grade_a_uploaded_ && frame_->grade) grade_tex_a_->release();
        texture_nv12_uv_->release();
        texture_nv12_y_->release();
        program_nv12_->release();
    } else {
        if (single_fade) {
            texture_->release();
        } else if (texture_second_valid_ && frame_ && frame_->has_transition()) {
            texture_b_->release();
        }
        if (grade_a_uploaded_ && frame_ && frame_->grade) grade_tex_a_->release();
        if (grade_b_uploaded_ && frame_ && frame_->grade_b) grade_tex_b_->release();
        texture_->release();
        program_->release();
    }
    vao_.release();
    vbo_.release();

    draw_viewer_background();
    draw_viewer_overlays();
}

void ViewerGL::draw_viewer_overlays() {
    if (overlay_flags_ == 0) return;

    const float vw = static_cast<float>(width());
    const float vh = static_cast<float>(height());
    if (vw < 8.0f || vh < 8.0f) return;
    const float aspect = tex_h_ > 0 ? static_cast<float>(tex_w_) / tex_h_ : 1.0f;
    const float va = vw / vh;
    const float qw = (scale_mode_ == ScaleMode::Fill) ? std::max(aspect / va, 1.0f)
                                                      : std::min(aspect / va, 1.0f);
    const float qh = (scale_mode_ == ScaleMode::Fill) ? std::max(va / aspect, 1.0f)
                                                      : std::min(va / aspect, 1.0f);
    const QRectF media((vw - qw * vw) / 2.0, (vh - qh * vh) / 2.0, qw * vw, qh * vh);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(rect());
    const ThemeTokens& t = tokens();

    if (overlay_flags_ & static_cast<unsigned>(Overlay::ThirdsGrid)) {
        QColor g = t.ink_faint;
        g.setAlpha(170);
        painter.setPen(QPen(g, 1.0));
        for (int i = 1; i < 3; ++i) {
            const qreal x = media.left() + media.width() * i / 3.0;
            painter.drawLine(QLineF(x, media.top(), x, media.bottom()));
            const qreal y = media.top() + media.height() * i / 3.0;
            painter.drawLine(QLineF(media.left(), y, media.right(), y));
        }
    }

    if (overlay_flags_ & static_cast<unsigned>(Overlay::SafeAreas)) {
        QColor sa = t.ink_muted;
        sa.setAlpha(150);
        painter.setPen(QPen(sa, 1.0));
        painter.setBrush(Qt::NoBrush);
        const QRectF title(media.left() + media.width() * 0.10, media.top() + media.height() * 0.10,
                           media.width() * 0.80, media.height() * 0.80);
        const QRectF action(media.left() + media.width() * 0.05, media.top() + media.height() * 0.05,
                            media.width() * 0.90, media.height() * 0.90);
        painter.drawRect(title);
        painter.drawRect(action);
    }

    if (overlay_flags_ & static_cast<unsigned>(Overlay::PlaybackBadge)) {
        QFont bf = painter.font();
        bf.setPointSizeF(8);
        bf.setBold(true);
        painter.setFont(bf);
        const QFontMetricsF bfm(bf);
        const QString label = playing_ ? tr("PLAYING") : tr("PAUSED");
        const QRectF pill(media.left() + 8.0, media.top() + 8.0,
                          bfm.horizontalAdvance(label) + 26.0, 20.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(t.surface_low);
        painter.drawRoundedRect(pill, 10.0, 10.0);
        const QColor dot = playing_ ? t.accent : t.ink_faint;
        painter.setBrush(dot);
        painter.drawEllipse(QPointF(pill.left() + 11.0, pill.center().y()), 3.5, 3.5);
        painter.setPen(t.ink);
        painter.drawText(pill.adjusted(20.0, 0.0, -6.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }

    painter.end();
}

void ViewerGL::draw_blank() {
    const ThemeTokens& t = tokens();
    QPainter painter(this);
    painter.fillRect(rect(), viewer_background_color());
    if (viewer_background_ == ViewerBackground::Checkerboard)
        paint_checkerboard(painter, rect());
    painter.setPen(QPen(t.border, 1.0));
    painter.setBrush(t.surface_low);
    const QRectF badge(4, 4, 64, 16);
    painter.drawRoundedRect(badge, 8, 8);
    painter.setPen(t.ink_muted);
    QFont f = painter.font();
    f.setPointSizeF(8);
    painter.setFont(f);
    painter.drawText(badge, Qt::AlignCenter, mode_ == ViewerMode::Source ? QStringLiteral("SOURCE")
                                                                          : QStringLiteral("PROGRAM"));

    const QPointF c = rect().center();
    const QPixmap mark_pm = raw_icon("film-strip").pixmap(24, 24);
    painter.drawPixmap(QPointF(c.x() - 12.0, c.y() - 52.0), mark_pm);

    QFont tf = painter.font();
    tf.setPointSizeF(12.5);
    tf.setBold(true);
    painter.setFont(tf);
    painter.setPen(t.ink);
    const QString title = mode_ == ViewerMode::Source
        ? tr("Select a clip to inspect")
        : tr("Nothing to preview yet");
    const QRectF title_box(c.x() - 220.0, c.y() - 18.0, 440.0, 22.0);
    painter.drawText(title_box, Qt::AlignHCenter | Qt::AlignTop, title);

    QFont hf = painter.font();
    hf.setPointSizeF(9);
    hf.setBold(false);
    painter.setFont(hf);
    painter.setPen(t.ink_faint);
    const QString hint = mode_ == ViewerMode::Source
        ? tr("Click a media clip in the Media Pool to load it here")
        : tr("Add clips to the timeline to build your cut");
    const QRectF hint_box(c.x() - 260.0, c.y() + 8.0, 520.0, 18.0);
    painter.drawText(hint_box, Qt::AlignHCenter | Qt::AlignTop, hint);
}

void ViewerGL::set_viewer_background(ViewerBackground background) {
    if (viewer_background_ == background) return;
    viewer_background_ = background;
    update();
}

QColor ViewerGL::viewer_background_color() const {
    switch (viewer_background_) {
        case ViewerBackground::White:      return QColor(0xE8, 0xE8, 0xE8);
        case ViewerBackground::Gray:       return QColor(0x5A, 0x5A, 0x5A);
        case ViewerBackground::Checkerboard:
            return QColor(0x0A, 0x0A, 0x0C);
        case ViewerBackground::Black:
        default:                           return QColor(0x0A, 0x0A, 0x0C);
    }
}

void ViewerGL::draw_viewer_background() {
    if (viewer_background_ != ViewerBackground::Checkerboard) return;
    const float vw = static_cast<float>(width());
    const float vh = static_cast<float>(height());
    if (vw < 8.0f || vh < 8.0f) return;
    if (!(texture_valid_ || nv12_valid_)) return;
    const float aspect = tex_h_ > 0 ? static_cast<float>(tex_w_) / tex_h_ : 1.0f;
    const float va = vw / vh;
    const float qw = (scale_mode_ == ScaleMode::Fill) ? std::max(aspect / va, 1.0f)
                                                      : std::min(aspect / va, 1.0f);
    const float qh = (scale_mode_ == ScaleMode::Fill) ? std::max(va / aspect, 1.0f)
                                                      : std::min(va / aspect, 1.0f);
    const QRectF media((vw - qw * vw) / 2.0, (vh - qh * vh) / 2.0, qw * vw, qh * vh);
    QPainterPath exterior;
    exterior.setFillRule(Qt::OddEvenFill);
    exterior.addRect(rect());
    exterior.addRect(media);
    QPainter painter(this);
    painter.setClipPath(exterior);
    paint_checkerboard(painter, rect());
}

}
