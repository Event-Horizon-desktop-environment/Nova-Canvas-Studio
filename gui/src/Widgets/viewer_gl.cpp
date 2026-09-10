#include "viewer_gl.hpp"
#include "Logging.hpp"
#include "UX/theme.hpp"

#include "canvas/core/gpu/colorspace.hpp"
#include "canvas/core/grade_graph/lut.hpp"
#include "canvas/core/util/color_log.hpp"
#include "canvas/core/util/log.hpp"

#include <QImage>
#include <QOpenGLContext>
#include <QPainter>
#include <QVector2D>

#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <utility>

namespace canvas::gui {

namespace {

// GLSL dialect header: Qt6 under the Wayland QPA presents an OpenGL ES context
// on many drivers (NVIDIA here hands out ES 3.2), whose shader translator
// rejects desktop `#version 330 core` fragment shaders that declare sampler3D
// ("global type sampler3D requires '#version 300' or later"). Emit ES-flavored
// source (`#version 300 es` + precision statements) when the context is ES so
// the NV12 LUT/transtion programs link there too; desktop GL keeps 330 core.
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

constexpr const char* kVertexSrc = R"(
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
out vec2 v_uv;
void main() {
    v_uv = in_uv;
    gl_Position = vec4(in_pos, 0.0, 1.0);
}
)";

// NV12 -> RGB conversion driven by the frame's per-file color spec (u_matrix,
// u_range), matching the CUDA composite kernel's frame-space convention
// (R=Y+1.402Cr, G=Y-0.344U-0.714V, B=Y+1.772U with Y/U/V centered at 0). The
// coefficient table below mirrors colorspace.hpp's MatrixCoeffs 1:1, so the GPU
// path and the CPU fallback/scopes agree on every (matrix, range) pairing. For
// BT.709 limited-range this is bit-identical to the historical hardcoded form
// (1.164*(Y-16) + 1.793Cr etc.); for OBS-style files tagged `tv` but carrying
// full-range data, u_range flips the decode to full-swing so highlights don't
// clip and skin tones don't shift lavender.
// u_tex_y samples the R8 luma plane, u_tex_uv the interleaved CbCr plane
// (.r = Cb, .g = Cr).
//
// Grade LUT: on the NV12 fast path a graded clip's bake arrives as a 3D RGB->RGB
// LUT texture (u_grade, RGB32F, trilinear). Resolve-style law: after the YUV->RGB
// conversion, sample at the texel-center coordinate (see grade_graph/lut.hpp).
// The bake's data is r-major (R slowest) but uploads to a GL 3D texture where x
// is fastest, so texel (x,y,z) holds gridpoint (r=z,g=y,b=x); the shader swaps
// R/B in the coordinate to evaluate gridpoint (r,g,b), matching the CPU
// `apply_grade_lut` that the scrub/export paths use — preview == export here.
// u_grade_size < 2 disables the pass (no grade attached).
constexpr const char* kFragNv12Src = R"(
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;
uniform sampler3D u_grade;        // baked 3D RGB->RGB grade LUT (RGB32F)
uniform int       u_grade_size;   // N (grid cells per axis); <2 => no grade
uniform int       u_matrix;       // 0=BT601 1=BT709 2=BT2020
uniform int       u_range;        // 0=limited 1=full
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
        // The bake is laid out r-major (R slowest index, B fastest) but the
        // upload hands the array to a GL 3D texture verbatim, where x is the
        // fastest axis: texel (x,y,z) holds gridpoint (r=z, g=y, b=x). Swap R/B
        // in the sample coordinate so input (r,g,b) hits gridpoint (r,g,b) —
        // exactly like the CPU apply_grade_lut the scrub/export paths use.
        vec3 coord = vec3(rgb.b, rgb.g, rgb.r) * float(u_grade_size - 1) / float(u_grade_size) + 0.5 / float(u_grade_size);
        rgb = texture(u_grade, coord).rgb;
    }
    fragColor = vec4(rgb, 1.0);
}
)";

// NV12 transition blend: converts BOTH Y/UV pairs (A and incoming B) to RGB and
// applies the same TransitionRenderMode math as kFragSrc. Samplers 2/3 read the
// B planes, which the CPU binds to A's planes when no B frame exists (single-
// clip fades) so MODE_FADEIN_A/FADEOUT never sample an unallocated texture.
//
// Grade LUTs: u_grade_a / u_grade_b are the 3D RGB->RGB bakes for the A and B
// clips (same Resolve-style texel-center convention as kFragNv12Src); each is
// applied to its clip's RGB BEFORE the blend math so a two-input transition
// grades both sides exactly like the exporter. Grade-b size < 2 disables a side
// (single-clip fades bind A's planes to the B slots — grade B is then null).
constexpr const char* kFragNv12Trans = R"(
uniform sampler2D u_tex_y;
uniform sampler2D u_tex_uv;      // A's interleaved CbCr
uniform sampler2D u_tex_b_y;
uniform sampler2D u_tex_b_uv;    // B's interleaved CbCr
uniform sampler3D u_grade_a;     // A's baked grade LUT (RGB32F)
uniform sampler3D u_grade_b;     // B's baked grade LUT (RGB32F)
uniform int    u_grade_a_size;   // N for A; <2 => no grade on A
uniform int    u_grade_b_size;   // N for B; <2 => no grade on B
uniform int    u_matrix_a;       // A's YUV matrix: 0=BT601 1=BT709 2=BT2020
uniform int    u_range_a;        // A's range: 0=limited 1=full
uniform int    u_matrix_b;       // B's YUV matrix
uniform int    u_range_b;        // B's range
uniform int    u_mode;           // TransitionRenderMode
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
    // Same r-major / x-fastest orientation as kFragNv12Src: the uploaded 3D
    // texture stores gridpoint (r=z, g=y, b=x) at texel (x,y,z), so swap R/B in
    // the coordinate to evaluate gridpoint (r,g,b) like the CPU path.
    vec3 coord = vec3(rgb.b, rgb.g, rgb.r) * float(size - 1) / float(size) + 0.5 / float(size);
    return vec4(texture(lut, coord).rgb, p.a);
}

void main() {
    vec4 a = grade_rgb(sample_yuv(u_tex_y, u_tex_uv, v_uv, u_matrix_a, u_range_a), u_grade_a, u_grade_a_size);
    if (u_mode == MODE_NONE) { fragColor = a; return; }
    vec4 b = grade_rgb(sample_yuv(u_tex_y, u_tex_uv, v_uv, u_matrix_b, u_range_b), u_grade_b, u_grade_b_size);
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

ViewerGL::~ViewerGL() {
    // QOpenGLTexture / QOpenGLShaderProgram members must be released while a GL
    // context is current; at app teardown none is, so Qt leaked each texture
    // handle and warned "destroy() called without a current context" per
    // resource. Make the widget's own context current for the cleanup.
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
        program_.reset();
        program_nv12_.reset();
        program_nv12_trans_.reset();
        vbo_.destroy();
        vao_.destroy();
        doneCurrent();
    }
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
    // whether a small preview frame needs the software upscale, and the
    // GUI-thread receive interval (`recv_ms`): the gap between successive
    // worker->widget frame handoffs. recv_ms near the [play] cadence = healthy
    // delivery; recv_ms far above it while cadence is fine = the GUI thread is
    // blocking between paints (busy signal handler, modal, slow repaint), a
    // stall class the worker-side cadence line cannot see.
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
        if (fw > 0 && fh > 0)
            qDebug() << "[viewer] set_frame"
                     << "frame=" << fw << "x" << fh
                       << "nv12=" << nw << "x" << nh
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height())
                       << "path=" << (frame->nv12 && !frame->nv12->y.empty() ? "nv12" : "rgba")
                       << "small=" << ((fw < width() || fh < height()) ? "yes" : "no")
                       << "last_tex=" << tex_w_ << "x" << tex_h_
                       << "recv_ms=" << QString::number(recv_ms, 'f', 1);
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
    nv12_b_valid_ = false;
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

    // NV12 (GPU composite fast path) needs its own program: two samplers (Y +
    // interleaved CbCr) instead of one RGBA texture.
    program_nv12_ = std::make_unique<QOpenGLShaderProgram>();
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertHdr + kVertexSrc);
    program_nv12_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragHdr + kFragNv12Src);
    program_nv12_->link();

    // Always-on: the NV12 fast path now decodes YUV->RGB with per-frame
    // u_matrix/u_range uniforms fed from each frame's RESOLVED spec (codecpar
    // tags reconciled with the full-range probe), so the GPU path and the CPU
    // swscale/scopes agree on every (matrix, range) — including OBS-style files
    // stamped `tv` that carry full-range data. The histogram of tags actually
    // seen per session is left to the frames' own [dec] open traces.
    ::canvas::core::log::log_warning(
        "[viewer] nv12 programs: per-frame u_matrix/u_range from Nv12Frame spec "
        "(tags+probe), not hardcoded 709-limited");

    // Four-sampler variant for transitions drawn from hardware planes (A + B).
    program_nv12_trans_ = std::make_unique<QOpenGLShaderProgram>();
    program_nv12_trans_->addShaderFromSourceCode(QOpenGLShader::Vertex, vertHdr + kVertexSrc);
    program_nv12_trans_->addShaderFromSourceCode(QOpenGLShader::Fragment, fragHdr + kFragNv12Trans);
    program_nv12_trans_->link();

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

    texture_nv12_b_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_b_y_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_y_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_y_->setWrapMode(QOpenGLTexture::ClampToEdge);

    texture_nv12_b_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
    texture_nv12_b_uv_->setMinificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
    texture_nv12_b_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);

    if (frame_) upload_frame();
}

void ViewerGL::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void ViewerGL::upload_frame() {
    if (!frame_ || !texture_) return;
    // Always-on ~1s upload telemetry: NV12 fast-path upload vs the RGBA (CPU
    // texture) fallback, with their average costs. A scrub that rides NV12 and
    // suddenly drops to rgba_upload means the fast path was lost — and nv12cvt
    // tallies the per-pixel YUV->RGB software conversions that re-appear then.
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

    // RGBA upload helper (owned-copy upscale + (re)allocate + setData). Defined
    // before the NV12 block so the small-frame CPU-conversion path below can
    // upload its result IN THE SAME PASS instead of deferring to a follow-up
    // paint (which continuous playback starves, leaving the viewer black).
    auto upload = [this](std::unique_ptr<QOpenGLTexture>& tex, const canvas::core::VideoFramePtr& f,
                     int& tw, int& th, bool& valid) {
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
                    upscaled = std::move(dst);   // keep pixels alive for setData
                    w = dw;
                    h = dh;
                    data = upscaled.constBits();
                    stride = static_cast<std::size_t>(dw) * 4;
                }
            }
        }

        const bool realloc = !tex->isStorageAllocated() || tw != w || th != h;
        if (realloc) ++up_realloc_cnt;
        static int64_t tex_log_ = 0;
        if ((tex_log_++ % 16) == 0)
            qDebug() << "[viewer] rgba_upload"
                       << "tex=" << w << "x" << h
                       << "src_frame=" << f->width << "x" << f->height
                       << "upscaled=" << (w != f->width || h != f->height ? "yes" : "no")
                       << "widget=" << std::max(1, width()) << "x" << std::max(1, height());
        if (realloc) {
            // Qt forbids setSize/setFormat once storage is allocated; a size
            // change needs a fresh texture object instead.
            tex = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            tex->setMinificationFilter(QOpenGLTexture::Linear);
            tex->setMagnificationFilter(QOpenGLTexture::Linear);
            tex->setWrapMode(QOpenGLTexture::ClampToEdge);
            tex->setSize(w, h);
            tex->setFormat(QOpenGLTexture::RGBA8_UNorm);
            tex->allocateStorage();
            tw = w;
            th = h;
        }
        tex->setData(0, 0, 0, w, h, 1, QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, data);
        valid = true;
    };

    // NV12 GPU fast path: upload the two planes as R8 (luma) + RG8 (CbCr)
    // textures; the per-frame-spec YUV->RGB conversion (u_matrix/u_range) is
    // applied in the fragment shader.
    if (frame_->nv12 && !frame_->nv12->y.empty()) {
        const canvas::core::Nv12Frame* n = frame_->nv12.get();
        const int w = n->width;
        const int h = n->height;

        // Small previews (scrub) don't magnify reliably in GL on some drivers, so
        // when the NV12 plane is smaller than the player, convert to CPU RGBA
        // and upload THAT now (same pass). Full-res playback keeps the fast NV12
        // texture path. Deferring the upload to a follow-up paint pass is a
        // black-frame under playback: a fresh NV12 frame re-arms the cvt before
        // the follow-up runs, so the viewer never draws anything but blank.
        if (w > 0 && h > 0 && (w < width() || h < height())) {
            // Always-on (once): this scrub/small-preview fallback converts NV12
            // via colorspace.hpp's yuv_to_rgb with the frame's RESOLVED per-file
            // spec (matrix + probe-reconciled range), so this CPU path and the
            // GPU shader AGREE on every (matrix, range) pair — including the
            // OBS-style full-range files whose `tv` tag lies.
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

            // Convert to RGBA, then apply the clip's grade LUT on the CPU so the
            // small-preview fallback shows (and scopes keep reading) the graded
            // pixels exactly as the GPU NV12 path would. The grade, if any, lives
            // on the RenderFrame (not the NV12 planes), so carry it over.
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
            if (frame_->grade && frame_->grade->valid()) {
                if (canvas::core::VideoFramePtr g = canvas::core::grade_graph::apply_grade_lut(*carry, *frame_->grade)) {
                    // CPU fallback grade trace: the small-preview (scrub) path
                    // re-grades on the CPU, so a grade that only ever shows in
                    // the preview still has a line here proving it ran.
                    // Gated like the sibling NV12->RGBA line — per-frame at
                    // scrub rate would wash the log out.
                    static int64_t cpugrade_log_ = 0;
                    if ((cpugrade_log_++ % 16) == 0) {
                        qDebug().nospace()
                            << "[grade] viewer CPU-grade apply size=" << frame_->grade->size
                            << " frame=" << (frame_->a ? frame_->a->frame_number : -1);
                    }
                    carry = std::move(g);
                }
            }
            rf->a = std::move(carry);
            rf->b = frame_->b;
            rf->mode = frame_->mode;
            rf->progress = frame_->progress;
            rf->fade_from_black = frame_->fade_from_black;
            rf->fade_to_black = frame_->fade_to_black;
            frame_ = std::move(rf);
            nv12_valid_ = false;
            // Upload in this same pass so playback never draws a blank frame.
            upload(texture_, frame_->a, tex_w_, tex_h_, texture_valid_);
            texture_second_valid_ = false;
            if (frame_->b && frame_->b->rgba.size() >= frame_->b->stride * frame_->b->height) {
                int bw = 0;
                int bh = 0;
                upload(texture_b_, frame_->b, bw, bh, texture_second_valid_);
            }
            ++up_cvt_cnt;
            texture_dirty_ = false;
            up_mark("r");
            return;
        }

        const bool y_realloc = !texture_nv12_y_->isStorageAllocated() || tex_w_ != w ||
                               tex_h_ != h;
        if (y_realloc) {
            ++up_realloc_cnt;
            // Qt forbids setSize/setFormat once storage is allocated (logs
            // "Cannot change format once storage has been allocated" and keeps
            // the stale buffer). Size changes therefore need a fresh texture
            // object; re-create the pair here (still on the context thread via
            // upload_frame's callers).
            texture_nv12_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            texture_nv12_y_->setMinificationFilter(QOpenGLTexture::Linear);
            texture_nv12_y_->setMagnificationFilter(QOpenGLTexture::Linear);
            texture_nv12_y_->setWrapMode(QOpenGLTexture::ClampToEdge);
            texture_nv12_y_->setSize(w, h);
            texture_nv12_y_->setFormat(QOpenGLTexture::R8_UNorm);
            texture_nv12_y_->allocateStorage();
            texture_nv12_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            texture_nv12_uv_->setMinificationFilter(QOpenGLTexture::Linear);
            texture_nv12_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
            texture_nv12_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);
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
        // Incoming (B) clip during an NV12 transition: upload its Y/UV pair too
        // so paintGL can blend both clips in kFragNv12Trans.
        nv12_b_valid_ = false;
        if (frame_->b_nv12 && !frame_->b_nv12->y.empty()) {
            const canvas::core::Nv12Frame* bn = frame_->b_nv12.get();
            const int bw = bn->width;
            const int bh = bn->height;
            const bool b_realloc = !texture_nv12_b_y_->isStorageAllocated() ||
                                   tex_bw_ != bw || tex_bh_ != bh;
            if (b_realloc) {
                ++up_realloc_cnt;
                texture_nv12_b_y_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
                texture_nv12_b_y_->setMinificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_y_->setMagnificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_y_->setWrapMode(QOpenGLTexture::ClampToEdge);
                texture_nv12_b_y_->setSize(bw, bh);
                texture_nv12_b_y_->setFormat(QOpenGLTexture::R8_UNorm);
                texture_nv12_b_y_->allocateStorage();
                texture_nv12_b_uv_ = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
                texture_nv12_b_uv_->setMinificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_uv_->setMagnificationFilter(QOpenGLTexture::Linear);
                texture_nv12_b_uv_->setWrapMode(QOpenGLTexture::ClampToEdge);
                texture_nv12_b_uv_->setSize(bw / 2, bh / 2);
                texture_nv12_b_uv_->setFormat(QOpenGLTexture::RG8_UNorm);
                texture_nv12_b_uv_->allocateStorage();
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
        // Upload the clip's grade LUT(s) as RGB32F 3D textures (unit 4/5 in the
        // shader). Only when the LUT pointer changed: rebaking is decode-side and
        // cached, and identical lookups across successive frames skip the upload.
        //
        // QOpenGLTexture forbids setSize/setFormat/allocateStorage once storage
        // is allocated, and re-allocating anyway is driver-dependent garbage
        // (can visibly corrupt the sampled LUT). So the immutable parts run only
        // on a fresh texture; later re-uploads are setData-only overwrites. The
        // grid is size-33 by default and cached decode-side, so grids rarely
        // change; a different size still rebuilds the texture object.
        {   // A-side LUT → grade_tex_a_
            const canvas::core::grade_graph::GradeLut3D* lut = frame_->grade.get();
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
                    << " frame=" << (frame_->a ? frame_->a->frame_number : -1)
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
        {   // B-side LUT → grade_tex_b_
            const canvas::core::grade_graph::GradeLut3D* lut = frame_->grade_b.get();
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
                    << " frame=" << (frame_->b ? frame_->b->frame_number : -1)
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
        // Keep second texture size so letterboxing matches texture A's aspect.
    }

    texture_dirty_ = false;
    up_mark("r");
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
        static auto log_t0 = std::chrono::steady_clock::now();
        static double sum_ms = 0.0;
        static int64_t sum_n = 0;
        // Measure this paint's wall time (upload + draw) so presentation cost is
        // attributable to the GL path, not just the decode that fed it.
        const auto p0 = std::chrono::steady_clock::now();
        if ((paint_log_++ % 30) == 0)
            qDebug() << "[viewer] paint"
                   << "mode=" << (scale_mode_ == ScaleMode::Fill ? "fill" : "fit")
                       << "tex=" << tex_w_ << "x" << tex_h_
                       << "widget=" << static_cast<int>(vw) << "x" << static_cast<int>(vh)
                       << "tex_aspect=" << aspect
                       << "widget_aspect=" << va
                       << "cover_x=" << qw << "cover_y=" << qh
                       << "path=" << (nv12_valid_ ? "nv12" : "rgba");
        const auto p1 = std::chrono::steady_clock::now();
        const double paint_ms =
            std::chrono::duration<double, std::milli>(p1 - p0).count();
        sum_ms += paint_ms;
        ++sum_n;
        // ~1/s aggregate paint cost: sustained ms here (well above ~8.3ms@60Hz)
        // means the viewer itself is the bottleneck once decode is healthy.
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

    // Single-clip edge fade (fade-in-from-black at the clip's head, or fade-out-
    // to-black at its tail) blends the A texture against black via u_mode/
    // u_progress. Rendering it needs no B texture.
    const bool single_fade = frame_ && (frame_->fade_from_black || frame_->fade_to_black);

    const bool nv12_cur = nv12_valid_ && frame_ && frame_->nv12;
    // NV12 BLEND: hardware planes also carry transitions and edge fades when
    // the timeline delivers them GPU-first (b_nv12 + mode/progress for
    // two-input transitions, or a single clip for fades). Preferred over both
    // the plain NV12 path and the RGBA path.
    const bool nv12_blend =
        nv12_cur &&
        (single_fade || frame_->mode != canvas::core::TransitionRenderMode::None);

    // Grade bind-state summary, logged only on CHANGE: proves a freshly baked
    // LUT actually reaches a texture AND is the texture currently sampled at
    // draw time. `drop` (grade present but not bound) is the state that would
    // draw an ungraded or stale-sampled frame; bake-hash vs upload-hash (above)
    // plus tex address here pin corruption to bake, upload, or bind.
    {
        const bool grade_present = frame_ && frame_->grade && frame_->grade->valid();
        const bool grade_bound = grade_present && grade_tex_a_ &&
                                 grade_a_uploaded_ == frame_->grade.get();
        // Re-log whenever a new LUT object goes live (pointer identity chases
        // every re-bake), not just on the present/bound/tex flags — those are
        // stable once the first grade binds, so a state-only gate would log
        // exactly once per session.
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
        texture_nv12_y_->bind(0);
        texture_nv12_uv_->bind(1);
        program_nv12_trans_->setUniformValue("u_tex_y", 0);
        program_nv12_trans_->setUniformValue("u_tex_uv", 1);
        const bool have_b = nv12_b_valid_ && frame_->b_nv12;
        if (have_b) {
            texture_nv12_b_y_->bind(2);
            texture_nv12_b_uv_->bind(3);
        } else {
            // Single-clip fade (no B frame): bind A's planes to the B slots so
            // MODE_FADEIN_A/FADEOUT never sample an unallocated texture.
            texture_nv12_y_->bind(2);
            texture_nv12_uv_->bind(3);
        }
        program_nv12_trans_->setUniformValue("u_tex_b_y", 2);
        program_nv12_trans_->setUniformValue("u_tex_b_uv", 3);
        // Per-side YUV color spec: each clip decodes with its own matrix/range
        // (probe-reconciled), and a single-clip fade's B slots alias A's planes
        // so B inherits A's spec there. Defaults live in the shader's runtime
        // branches — never rely on the GL default (0 = BT601 limited).
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
        // Grade LUTs: A lives on unit 4, B on unit 5. A single-clip fade has no
        // B grade (B slots alias A's planes), so grade_b stays disabled there.
        const bool grade_a_attached = frame_->grade && frame_->grade->valid() &&
                                      grade_tex_a_ && grade_a_uploaded_ == frame_->grade.get();
        const bool grade_b_attached = have_b && frame_->grade_b &&
                                      frame_->grade_b->valid() && grade_tex_b_ &&
                                      grade_b_uploaded_ == frame_->grade_b.get();
        if (grade_a_attached) {
            grade_tex_a_->bind(4);
            program_nv12_trans_->setUniformValue("u_grade_a", 4);
            program_nv12_trans_->setUniformValue("u_grade_a_size", frame_->grade->size);
        } else {
            program_nv12_trans_->setUniformValue("u_grade_a_size", 0);
        }
        if (grade_b_attached) {
            grade_tex_b_->bind(5);
            program_nv12_trans_->setUniformValue("u_grade_b", 5);
            program_nv12_trans_->setUniformValue("u_grade_b_size", frame_->grade_b->size);
        } else {
            program_nv12_trans_->setUniformValue("u_grade_b_size", 0);
        }
        if (single_fade) {
            const int fade_mode = frame_->fade_from_black ? 9 /*MODE_FADEIN_A*/
                                                          : 3 /*MODE_FADEOUT*/;
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
        texture_nv12_y_->bind(0);
        texture_nv12_uv_->bind(1);
        program_nv12_->setUniformValue("u_tex_y", 0);
        program_nv12_->setUniformValue("u_tex_uv", 1);
        // This clip decodes with its own resolved matrix/range (see the trans
        // path for the same pair); GL's default uniform is 0 = BT601 limited,
        // so set it every frame.
        program_nv12_->setUniformValue("u_matrix", static_cast<int>(frame_->nv12->matrix));
        program_nv12_->setUniformValue("u_range", static_cast<int>(frame_->nv12->range));
        // A-side grade LUT (unit 4); the plain NV12 shader has no B side.
        const bool grade_a_attached = frame_->grade && frame_->grade->valid() &&
                                      grade_tex_a_ && grade_a_uploaded_ == frame_->grade.get();
        // Color archive: [viewer] line on spec/grade changes only, so wheel/curve
        // interactions show when a grade actually attached to the preview draw.
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
        if (grade_a_attached) {
            grade_tex_a_->bind(4);
            program_nv12_->setUniformValue("u_grade", 4);
            program_nv12_->setUniformValue("u_grade_size", frame_->grade->size);
        } else {
            program_nv12_->setUniformValue("u_grade_size", 0);
        }
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

    if (nv12_blend) {
        if (grade_a_uploaded_ && frame_->grade) grade_tex_a_->release();
        if (grade_b_uploaded_ && frame_->grade_b) grade_tex_b_->release();
        if (nv12_b_valid_ && frame_->b_nv12) {
            texture_nv12_b_uv_->release();
            texture_nv12_b_y_->release();
        } else {
            texture_nv12_uv_->release();  // bound to both slots 1 and 3 (B fallback)
            texture_nv12_y_->release();   // bound to both slots 0 and 2 (B fallback)
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
    const ThemeTokens& t = tokens();
    QPainter painter(this);
    painter.fillRect(rect(), t.surface);
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
}

}