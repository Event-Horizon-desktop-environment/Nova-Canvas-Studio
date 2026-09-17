#pragma once

#include "canvas/core/media/vaapi/surface.hpp"

#include <QOpenGLFunctions>

#include <memory>

namespace canvas::core::vaapi {
struct VaapiSurface;
}

namespace canvas::gui {

class VaapiViewerImporter final {
public:
    VaapiViewerImporter();
    ~VaapiViewerImporter();
    VaapiViewerImporter(const VaapiViewerImporter&) = delete;
    VaapiViewerImporter& operator=(const VaapiViewerImporter&) = delete;

    [[nodiscard]] bool available();

    [[nodiscard]] bool import(const canvas::core::vaapi::VaapiSurface& surf,
                              GLuint* out_y, GLuint* out_uv);

    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
