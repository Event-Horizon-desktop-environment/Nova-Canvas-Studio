// Zero-copy VAAPI surface importer (see the header for the design contract).

#include "Widgets/vaapi_viewer.hpp"

#include "canvas/core/media/vaapi/driver.hpp"
#include "canvas/core/media/vaapi/surface.hpp"
#include "canvas/core/util/log.hpp"

#include <QOpenGLContext>
#include <QOpenGLFunctions>

#include <cstring>

#if defined(CANVAS_HAVE_VAAPI) && defined(CANVAS_HAVE_EGL)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unistd.h>
#endif

namespace canvas::gui {

VaapiViewerImporter::VaapiViewerImporter() = default;
VaapiViewerImporter::~VaapiViewerImporter() = default;

// ── Builds with both VAAPI and EGL: real EGLImage dmabuf import ────────────
#if defined(CANVAS_HAVE_VAAPI) && defined(CANVAS_HAVE_EGL)

namespace {

// One plane imported as an EGLImage. The EGL spec does NOT take ownership of
// the caller's fd — the caller keeps it open for the image's lifetime — so
// every plane gets its own dup of the export fd. `*out_fd` is that owned dup,
// closed on teardown (dma-buf import into two EGLImages sharing one fd is
// undefined, so the dup per image is required, not defensive).
bool make_plane_image(EGLDisplay dpy, int w, int h, std::uint32_t fourcc,
                      const canvas::core::vaapi::VaapiObject& obj,
                      const canvas::core::vaapi::VaapiPlane& pl, std::uint64_t mod,
                      bool pass_modifier, EGLImage* out_img, int* out_fd) {
    int fd = dup(obj.fd);
    if (fd < 0) {
        ::canvas::core::log::log_warning("[vaapi] import: dup(fd=%d) failed", obj.fd);
        return false;
    }
    EGLAttrib a[64];
    int i = 0;
    a[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    a[i++] = static_cast<EGLint>(fourcc);
    a[i++] = EGL_WIDTH;
    a[i++] = w;
    a[i++] = EGL_HEIGHT;
    a[i++] = h;
    if (pass_modifier && mod != 0) {
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        a[i++] = static_cast<EGLint>(mod & 0xffffffffu);
        a[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        a[i++] = static_cast<EGLint>((mod >> 32) & 0xffffffffu);
    }
    a[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    a[i++] = fd;
    a[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    a[i++] = static_cast<EGLint>(pl.offset);
    a[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    a[i++] = static_cast<EGLint>(pl.pitch);
    a[i++] = EGL_NONE;

    EGLImage img = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                  nullptr, a);
    if (img == EGL_NO_IMAGE) {
        ::canvas::core::log::log_warning(
            "[vaapi] import plane fourcc=%c%c%c%c w=%d h=%d mod=%s egl_error=0x%x",
            static_cast<char>((fourcc >> 24) & 0xff), static_cast<char>((fourcc >> 16) & 0xff),
            static_cast<char>((fourcc >> 8) & 0xff), static_cast<char>(fourcc & 0xff), w, h,
            (pass_modifier && mod != 0) ? "set" : "linear",
            static_cast<unsigned>(eglGetError()));
        close(fd);
        return false;
    }
    *out_img = img;
    *out_fd = fd;
    return true;
}

// make_plane_image plus the vendor linear-fallback rule: a modifier-specified
// import can fail where a LINEAR retry succeeds (iHD-era quirk). A LINEAR
// request itself is never second-guessed.
bool make_plane_image_retry(
    EGLDisplay dpy, int w, int h, std::uint32_t fourcc,
    const canvas::core::vaapi::VaapiObject& obj, const canvas::core::vaapi::VaapiPlane& pl,
    std::uint64_t mod, bool pass_modifier, const canvas::core::vaapi::ImportPolicy& pol,
    EGLImage* out_img, int* out_fd) {
    if (make_plane_image(dpy, w, h, fourcc, obj, pl, mod, pass_modifier, out_img, out_fd))
        return true;
    if (pass_modifier && mod != 0 && pol.modifiers_retry_linear) {
        ::canvas::core::log::log_warning(
            "[vaapi] import plane fourcc=%c%c%c%c retrying LINEAR",
            static_cast<char>((fourcc >> 24) & 0xff), static_cast<char>((fourcc >> 16) & 0xff),
            static_cast<char>((fourcc >> 8) & 0xff), static_cast<char>(fourcc & 0xff));
        return make_plane_image(dpy, w, h, fourcc, obj, pl, mod, false, out_img, out_fd);
    }
    return false;
}

// Binds one texture to an EGLImage with the NV12-path sampling state (linear
// filter, clamp-to-edge), via glEGLImageTargetTexture2DOES. After this the
// texture object resolves the image on every regular glBindTexture.
void fn_bind_param(QOpenGLFunctions* f, GLuint tex, EGLImage img,
                   void (*image_target)(GLenum, GLeglImageOES)) {
    f->glBindTexture(GL_TEXTURE_2D, tex);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target(GL_TEXTURE_2D, img);
}

}  // namespace

struct VaapiViewerImporter::Impl {
    bool probed_ = false;
    bool available_ = false;
    QOpenGLFunctions* fn_ = nullptr;
    // glEGLImageTargetTexture2DOES (fetched via getProcAddress; typed so a
    // cast of the void-returning QFunctionPointer is unambiguous).
    void (*image_target_)(GLenum, GLeglImageOES) = nullptr;
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    // Live EGL images + the owned dup fds they reference (closed on teardown).
    EGLImage y_img_ = EGL_NO_IMAGE;
    EGLImage uv_img_ = EGL_NO_IMAGE;
    int y_fd_ = -1;
    int uv_fd_ = -1;
    // Raw texture ids targeted at the images (QOpenGLTexture cannot wrap them).
    GLuint y_tex_ = 0;
    GLuint uv_tex_ = 0;
    // Layout key of the live import: recreate images when any of these change.
    int w_ = 0;
    int h_ = 0;
    int src_fd_ = -1;  // the (non-dup) export fd the objects carried
    std::uint64_t mod_ = 0;
    std::uint32_t y_off_ = 0;
    std::uint32_t y_pitch_ = 0;
    std::uint32_t uv_off_ = 0;
    std::uint32_t uv_pitch_ = 0;
    bool mods_ext_ = false;

    void destroy_images() {
        if (y_img_ != EGL_NO_IMAGE) {
            eglDestroyImage(dpy_, y_img_);
            y_img_ = EGL_NO_IMAGE;
        }
        if (uv_img_ != EGL_NO_IMAGE) {
            eglDestroyImage(dpy_, uv_img_);
            uv_img_ = EGL_NO_IMAGE;
        }
        if (y_fd_ >= 0) {
            close(y_fd_);
            y_fd_ = -1;
        }
        if (uv_fd_ >= 0) {
            close(uv_fd_);
            uv_fd_ = -1;
        }
    }

    bool probe() {
        if (probed_) return available_;
        probed_ = true;
        QOpenGLContext* ctx = QOpenGLContext::currentContext();
        if (!ctx) return false;
        fn_ = ctx->functions();
        if (!fn_) return false;
        dpy_ = eglGetCurrentDisplay();
        if (dpy_ == EGL_NO_DISPLAY) return false;
        const char* exts = eglQueryString(dpy_, EGL_EXTENSIONS);
        if (!exts || !std::strstr(exts, "EGL_EXT_image_dma_buf_import")) return false;
        mods_ext_ = std::strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        image_target_ = reinterpret_cast<void (*)(GLenum, GLeglImageOES)>(
            ctx->getProcAddress("glEGLImageTargetTexture2DOES"));
        if (!image_target_) return false;
        available_ = true;
        return true;
    }
};

bool VaapiViewerImporter::available() {
    return impl_ && impl_->probe();
}

bool VaapiViewerImporter::import(const canvas::core::vaapi::VaapiSurface& surf, GLuint* out_y,
                                 GLuint* out_uv) {
    if (out_y) *out_y = 0;
    if (out_uv) *out_uv = 0;
    if (!impl_ || !impl_->probe()) return false;
    Impl* q = impl_.get();

    if (!surf.valid() || surf.planes.size() < 2 || surf.objects.empty()) {
        ::canvas::core::log::log_warning("[vaapi] import: surface invalid planes=%zu objects=%zu",
                                         surf.planes.size(), surf.objects.size());
        return false;
    }
    const canvas::core::vaapi::VaapiPlane& yp = surf.planes[0];
    const canvas::core::vaapi::VaapiPlane& up = surf.planes[1];
    if (yp.object_index >= surf.objects.size() || up.object_index >= surf.objects.size()) {
        ::canvas::core::log::log_warning("[vaapi] import: plane object index out of range");
        return false;
    }
    const canvas::core::vaapi::VaapiObject& y_obj = surf.objects[yp.object_index];
    const canvas::core::vaapi::VaapiObject& u_obj = surf.objects[up.object_index];
    const canvas::core::vaapi::ImportPolicy& pol =
        canvas::core::vaapi::import_policy(surf.vendor);
    const bool pass_modifier = q->mods_ext_ && pol.modifiers_supported && !pol.force_linear;

    // Unchanged DRM layout on a live import: the images are still bound, the
    // textures still resolve them — return the same ids without re-importing.
    if (q->y_img_ != EGL_NO_IMAGE && q->uv_img_ != EGL_NO_IMAGE &&
        q->w_ == surf.width && q->h_ == surf.height && q->src_fd_ == y_obj.fd &&
        q->mod_ == y_obj.modifier && yp.offset == q->y_off_ &&
        yp.pitch == q->y_pitch_ && up.offset == q->uv_off_ && up.pitch == q->uv_pitch_) {
        if (out_y) *out_y = q->y_tex_;
        if (out_uv) *out_uv = q->uv_tex_;
        return true;
    }

    // Layout changed (or first import): rebuild the images from scratch.
    q->destroy_images();
    if (q->y_tex_ == 0) {
        q->fn_->glGenTextures(1, &q->y_tex_);
        q->fn_->glGenTextures(1, &q->uv_tex_);
    }

    EGLImage y_img = EGL_NO_IMAGE;
    EGLImage u_img = EGL_NO_IMAGE;
    int y_fd = -1;
    int u_fd = -1;
    const std::uint64_t y_mod = y_obj.modifier;
    const std::uint64_t u_mod = (up.object_index == yp.object_index) ? y_obj.modifier
                                                                     : u_obj.modifier;
    if (!make_plane_image_retry(q->dpy_, surf.width, surf.height,
                                canvas::core::vaapi::kDrmFourccR8, y_obj, yp, y_mod,
                                pass_modifier, pol, &y_img, &y_fd)) {
        return false;
    }
    if (!make_plane_image_retry(q->dpy_, surf.width / 2, surf.height / 2,
                                canvas::core::vaapi::kDrmFourccRg88, u_obj, up, u_mod,
                                pass_modifier, pol, &u_img, &u_fd)) {
        eglDestroyImage(q->dpy_, y_img);
        close(y_fd);
        return false;
    }

    // Target both images at the raw textures (GL_EGL_image_target semantics:
    // the texture object then resolves the image on every bind).
    const auto image_target = reinterpret_cast<void (*)(GLenum, GLeglImageOES)>(
        q->image_target_);
    fn_bind_param(q->fn_, q->y_tex_, y_img, image_target);
    fn_bind_param(q->fn_, q->uv_tex_, u_img, image_target);
    const GLenum ge = q->fn_->glGetError();
    if (ge != GL_NO_ERROR) {
        ::canvas::core::log::log_warning(
            "[vaapi] import: glEGLImageTargetTexture2DOES gl_error=0x%x", static_cast<unsigned>(ge));
        eglDestroyImage(q->dpy_, y_img);
        eglDestroyImage(q->dpy_, u_img);
        close(y_fd);
        close(u_fd);
        return false;
    }

    q->y_img_ = y_img;
    q->uv_img_ = u_img;
    q->y_fd_ = y_fd;
    q->uv_fd_ = u_fd;
    q->w_ = surf.width;
    q->h_ = surf.height;
    q->src_fd_ = y_obj.fd;
    q->mod_ = y_obj.modifier;
    q->y_off_ = yp.offset;
    q->y_pitch_ = yp.pitch;
    q->uv_off_ = up.offset;
    q->uv_pitch_ = up.pitch;

    ::canvas::core::log::log_info("[vaapi] import surface %dx%d fd=%d mod=%s vendor=%s",
                                  surf.width, surf.height, y_obj.fd, y_mod ? "set" : "linear",
                                  canvas::core::vaapi::vendor_name(surf.vendor));
    if (out_y) *out_y = q->y_tex_;
    if (out_uv) *out_uv = q->uv_tex_;
    return true;
}

void VaapiViewerImporter::release() {
    if (!impl_) return;
    Impl* q = impl_.get();
    q->destroy_images();
    if (q->fn_ && q->y_tex_) {
        q->fn_->glDeleteTextures(1, &q->y_tex_);
        q->y_tex_ = 0;
    }
    if (q->fn_ && q->uv_tex_) {
        q->fn_->glDeleteTextures(1, &q->uv_tex_);
        q->uv_tex_ = 0;
    }
    q->probed_ = false;
    q->available_ = false;
}

// ── Build without VAAPI or EGL: the importer is a stub that reports no ─────
// ── capability, so the decode side gate falls back to CPU paths. ───────────
#else

struct VaapiViewerImporter::Impl {
    bool probe() { return false; }
};

bool VaapiViewerImporter::available() { return false; }

bool VaapiViewerImporter::import(const canvas::core::vaapi::VaapiSurface&, GLuint* out_y,
                                 GLuint* out_uv) {
    if (out_y) *out_y = 0;
    if (out_uv) *out_uv = 0;
    return false;
}

void VaapiViewerImporter::release() {}

#endif  // CANVAS_HAVE_VAAPI && CANVAS_HAVE_EGL

}  // namespace canvas::gui