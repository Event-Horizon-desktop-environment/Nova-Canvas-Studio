#pragma once

// Zero-copy VAAPI surface importer (gui-side, Qt + EGL).
//
// The decode worker hands the viewer a VaapiSurface: exported dmabuf fds plus
// the per-plane offsets/pitches the GL importer needs. This module owns the
// EGL side of the zero-copy path — it imports each plane as an EGLImage
// (plane[0] as an R8 image, plane[1] as an interleaved RG88 image, each on its
// own dup of the underlying object fd) and targets those images at two raw GL
// textures the drawn quad samples exactly like the CPU-NV12 upload path.
//
// Qt 6's QOpenGLTexture cannot wrap a texture populated by
// glEGLImageTargetTexture2DOES (no GLuint constructor on this Qt build), so
// the importer manages two raw texture ids directly. Every draw path that
// binds the CPU NV12 pair binds these ids instead when the frame is GPU-backed.
//
// Fallback contract: any import failure (no EGL, no dmabuf extension, a
// driver that rejects the modifier) is reported — the caller keeps the CPU
// NV12/RGBA upload path. The decode worker is additionally gated on the
// one-time import availability probe (see vaapi_import_state), so a session
// that cannot import never produces a GPU-only frame in the first place.

#include "canvas/core/media/vaapi/surface.hpp"

#include <QOpenGLFunctions>

#include <memory>

namespace canvas::core::vaapi {
struct VaapiSurface;
}  // namespace canvas::core::vaapi

namespace canvas::gui {

class VaapiViewerImporter final {
public:
    VaapiViewerImporter();
    ~VaapiViewerImporter();
    VaapiViewerImporter(const VaapiViewerImporter&) = delete;
    VaapiViewerImporter& operator=(const VaapiViewerImporter&) = delete;

    // True when the current EGL session can import dmabufs: an EGL display is
    // current, EGL_EXT_image_dma_buf_import is advertised, and the GL context
    // resolves glEGLImageTargetTexture2DOES. Probes once; cached for the
    // lifetime of this object. Must be called with a current QOpenGLContext
    // (the probe derives the EGL display from the thread-current one).
    [[nodiscard]] bool available();

    // Imports `surf`'s luma+chroma planes. Returns the two texture ids to
    // sample in *out_y / *out_uv and true on success. When the surface's DRM
    // layout (object fd, format modifier, plane offsets/pitches) matches an
    // already-live import, the existing images are reused and the same ids
    // returned (no re-import); a layout change tears the old images down and
    // builds new ones. Ids are zeroed on failure. Must run on the GL thread
    // with a current context.
    [[nodiscard]] bool import(const canvas::core::vaapi::VaapiSurface& surf,
                              GLuint* out_y, GLuint* out_uv);

    // Destroys the EGL images + textures. Safe to call multiple times. Needs a
    // current GL context (texture deletion); images are EGL-side and survive
    // without one.
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace canvas::gui