#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QColor>

#include <chrono>
#include <memory>

#include "canvas/core/media/frame.hpp"
#include "features/timeline/timeline_view_options.hpp"
#include "Widgets/vaapi_viewer.hpp"

namespace canvas::core::grade_graph {
struct GradeLut3D;
}

namespace canvas::gui {

class ViewerGL final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    enum class ViewerMode { Source, Program };

    enum class ScaleMode { Fit, Fill };

    explicit ViewerGL(QWidget* parent = nullptr);
    ~ViewerGL() override;

    void set_frame(canvas::core::RenderFramePtr frame);
    void clear();
    void set_mode(ViewerMode mode);
    void set_scale_mode(ScaleMode mode);
    void set_viewer_background(ViewerBackground background);
    [[nodiscard]] ViewerBackground viewer_background() const { return viewer_background_; }

    enum class Overlay : unsigned {
        SafeAreas = 1u << 0,
        ThirdsGrid = 1u << 1,
        PlaybackBadge = 1u << 2,
    };
    void set_overlay(Overlay overlay, bool on);
    [[nodiscard]] bool overlay_enabled(Overlay overlay) const;
    void set_playing(bool playing);
    [[nodiscard]] bool playing() const { return playing_; }

    [[nodiscard]] ViewerMode mode() const { return mode_; }
    [[nodiscard]] ScaleMode scale_mode() const { return scale_mode_; }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void upload_frame();
    void bind_nv12_a(int y_unit, int uv_unit);
    void bind_nv12_b(int y_unit, int uv_unit);
    void bind_grade_lut(int unit, QOpenGLTexture* lut);
    void draw_blank();
    void draw_viewer_overlays();
    void draw_viewer_background();
    [[nodiscard]] QColor viewer_background_color() const;

    canvas::core::RenderFramePtr frame_;
    ViewerMode mode_ = ViewerMode::Program;
    ScaleMode scale_mode_ = ScaleMode::Fit;
    ViewerBackground viewer_background_ = ViewerBackground::Black;
    unsigned overlay_flags_ = 0;
    bool playing_ = false;

    std::unique_ptr<QOpenGLTexture> texture_;
    std::unique_ptr<QOpenGLTexture> texture_b_;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_y_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_uv_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_b_y_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_b_uv_;
    std::unique_ptr<QOpenGLShaderProgram> program_nv12_;
    std::unique_ptr<QOpenGLShaderProgram> program_nv12_trans_;
    std::unique_ptr<QOpenGLTexture> grade_tex_a_;
    std::unique_ptr<QOpenGLTexture> grade_tex_b_;
    GLuint grade_neutral_tex_ = 0;
    const canvas::core::grade_graph::GradeLut3D* grade_a_uploaded_ = nullptr;
    const canvas::core::grade_graph::GradeLut3D* grade_b_uploaded_ = nullptr;
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject vao_;
    GLint attr_pos_ = -1;
    GLint attr_uv_ = -1;
    GLint uni_mode_ = -1;
    GLint uni_progress_ = -1;
    GLint uni_aspect_ = -1;
    int tex_w_ = 0;
    int tex_h_ = 0;
    int tex_bw_ = 0;
    int tex_bh_ = 0;
    uint64_t viewer_uid_ = 0;
    bool texture_valid_ = false;
    bool texture_second_valid_ = false;
    bool texture_dirty_ = false;
    bool nv12_valid_ = false;
    bool nv12_b_valid_ = false;
    std::unique_ptr<VaapiViewerImporter> vaapi_importer_;
    std::unique_ptr<VaapiViewerImporter> vaapi_importer_b_;
    GLuint vaapi_tex_y_ = 0;
    GLuint vaapi_tex_uv_ = 0;
    GLuint vaapi_tex_b_y_ = 0;
    GLuint vaapi_tex_b_uv_ = 0;
    bool vaapi_valid_ = false;
    bool vaapi_b_valid_ = false;
    bool rgba_gl_ok_ = true;
    std::chrono::steady_clock::time_point last_frame_arrival_{};
    bool have_last_arrival_ = false;

    canvas::core::gpu::ColorMatrix last_spec_matrix_ = canvas::core::gpu::ColorMatrix::BT709;
    canvas::core::gpu::ColorRange last_spec_range_ = canvas::core::gpu::ColorRange::Limited;
    int last_grade_attached_ = -1;
    bool last_spec_set_ = false;
};

}