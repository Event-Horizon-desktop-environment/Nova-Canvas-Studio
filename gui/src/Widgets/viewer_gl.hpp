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
// (or NV12 planes) and are uploaded into reused OpenGL textures, then drawn
// as a textured quad with GPU scaling (avoids per-frame CPU QImage copy +
// software scaling).
class ViewerGL final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    enum class ViewerMode { Source, Program };

    // How the frame is scaled into the media window: Fit shows the whole frame
    // (letterbox bars on the odd axis), Fill covers the window edge-to-edge by
    // cropping the overflow axis.
    enum class ScaleMode { Fit, Fill };

    explicit ViewerGL(QWidget* parent = nullptr);

    void set_frame(canvas::core::RenderFramePtr frame);
    void clear();
    void set_mode(ViewerMode mode);
    void set_scale_mode(ScaleMode mode);

    // Optional editor overlays drawn over the presented frame: action/title
    // safe areas, a rule-of-thirds grid, and a live playback indicator. They
    // are toggled from the viewer's right-click menu and persisted in QSettings
    // (see ShellCenter.cpp), and are purely cosmetic — never baked to export.
    enum class Overlay : unsigned {
        SafeAreas = 1u << 0,   // 90% action-safe / 80% title-safe boxes
        ThirdsGrid = 1u << 1,  // rule-of-thirds guides
        PlaybackBadge = 1u << 2,
    };
    void set_overlay(Overlay overlay, bool on);
    [[nodiscard]] bool overlay_enabled(Overlay overlay) const;
    // Drives the playback indicator badge (MainWindow's on_playback_changed
    // keeps it in lockstep with the SequenceController's real state).
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
    void draw_blank();
    // Paints the monitor overlays (safe areas / thirds grid / playback badge)
    // after the frame quad, in widget coordinates and clipped to the widget.
    void draw_viewer_overlays();

    canvas::core::RenderFramePtr frame_;
    ViewerMode mode_ = ViewerMode::Program;
    ScaleMode scale_mode_ = ScaleMode::Fit;
    // All overlays default OFF: the monitor stays a clean picture unless the
    // operator opts in (Guides button in the top bar, or the viewer's
    // right-click menu). ShellCenter reads QSettings and overrides before
    // first paint.
    unsigned overlay_flags_ = 0;
    bool playing_ = false;

    std::unique_ptr<QOpenGLTexture> texture_;
    std::unique_ptr<QOpenGLTexture> texture_b_;
    std::unique_ptr<QOpenGLShaderProgram> program_;
    // NV12 GPU fast path: Y uploaded as an R8 texture, interleaved CbCr as an
    // RG8 texture, converted to RGB in kFragNv12Src (BT.601 limited). The
    // second Y/UV pair backs the incoming (B) clip during an NV12 transition,
    // blended by kFragNv12Trans.
    std::unique_ptr<QOpenGLTexture> texture_nv12_y_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_uv_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_b_y_;
    std::unique_ptr<QOpenGLTexture> texture_nv12_b_uv_;
    std::unique_ptr<QOpenGLShaderProgram> program_nv12_;
    std::unique_ptr<QOpenGLShaderProgram> program_nv12_trans_;
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
    bool texture_valid_ = false;
    bool texture_second_valid_ = false;
    bool texture_dirty_ = false;
    bool nv12_valid_ = false;
    bool nv12_b_valid_ = false;
};

}
