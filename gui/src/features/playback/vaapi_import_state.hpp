#pragma once

// Zero-copy VAAPI import availability flag (headless).
//
// The decode worker decides whether a VAAPI frame may be produced GPU-only
// (exported dmabufs, no CPU planes) based on whether the GL viewer can import
// them: EGLImage dmabuf import is required, and it is not guaranteed (GLX-only
// sessions, missing EGL_EXT_image_dma_buf_import, a GLSL/EGL driver without
// glEGLImageTargetTexture2DOES). A GPU-only frame has no CPU fallback on the
// viewer side, so the producer must not create one unless the consumer can
// display it.
//
// ViewerGL probes once (initializeGL, before frames flow) and publishes the
// result here; the timeline decode worker reads it per call. When false, the
// VAAPI branch of decode_nv12_slot returns null and the existing CPU RGBA/
// NV12 fallback runs — the same degrade path an export failure already takes.
//
// Qt-free by design so timeline_decoder.cpp (headless) can read it.

namespace canvas::gui {

// True once ViewerGL (or any EGL-capable owner) has published that EGLImage
// dmabuf import works on the active GL/EGL session.
[[nodiscard]] bool vaapi_viewer_import_available() noexcept;

// Published by ViewerGL after its one-time probe in initializeGL. Safe to call
// from any thread; the decode worker reads the last published value.
void set_vaapi_viewer_import_available(bool available) noexcept;

}  // namespace canvas::gui