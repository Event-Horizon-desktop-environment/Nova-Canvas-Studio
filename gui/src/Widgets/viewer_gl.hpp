#pragma once

#include <QImage>
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>

#include <memory>

#include "canvas/core/media/frame.hpp"

namespace canvas::gui {

// Hardware-accelerated video viewer. Frames arrive as CPU-side RGBA buffers
// (downloaded from the GPU by VideoDecoder) and are uploaded into a single
// reused OpenGL texture, then drawn as a textured quad with GPU scaling. This
// removes the per-frame CPU QImage copy + software scaling that caused frame
// drops at high resolutions, leaving scaling to the GPU.
class ViewerGL final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    enum class ViewerMode { Source, Program };

    // How the frame is scaled into the media window. Fit shows the whole frame
    // (letterbox bars on the odd axis, Kdenlive/Resolve default). Fill covers the
    // window edge-to-edge, cropping the overflow axis of the source.
    enum class ScaleMode { Fit, Fill };

    explicit ViewerGL(QWidget* parent = nullptr);

    void set_frame(canvas::core::RenderFramePtr frame);
    void clear();
    void set_mode(ViewerMode mode);
    void set_scale_mode(ScaleMode mode);

    [[nodiscard]] ViewerMode mode() const { return mode_; }
    [[nodiscard]] ScaleMode scale_mode() const { return scale_mode_; }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void upload_frame();
    void draw_blank();

    canvas::core::RenderFramePtr frame_;
    ViewerMode mode_ = ViewerMode::Program;
    ScaleMode scale_mode_ = ScaleMode::Fit;

    std::unique_ptr<QOpenGLTexture> texture_;
    std::unique_ptr<QOpenGLTexture> texture_b_;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    // NV12 GPU fast path: Y uploaded as an R8 texture, interleaved CbCr as an
    // RG8 texture, converted to RGB in kFragNv12Src (BT.601 limited).
    std::unique_ptr<QOpenGLTexture> texture_nv12_y_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_uv_;
    std::unique_ptr<QOpenGLShaderProgram> program_nv12_;
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
    QOpenGLVertexArrayObject vao_;
    GLint attr_pos_ = -1;
    GLint attr_uv_ = -1;
    GLint uni_mode_ = -1;
    GLint uni_progress_ = -1;
    GLint uni_aspect_ = -1;
    int tex_w_ = 0;
    int tex_h_ = 0;
    bool texture_valid_ = false;
    bool texture_second_valid_ = false;
    bool texture_dirty_ = false;
    bool nv12_valid_ = false;
};

}
