#pragma once

#include "canvas/core/export/exporter.hpp"
#include "canvas/core/timeline/model.hpp"

#include <string_view>
#include <vector>

namespace canvas::core::chapters {

[[nodiscard]] bool container_supports(std::string_view format);

[[nodiscard]] std::vector<ExportChapter> for_export(const Sequence& seq, bool enabled);

void apply(ExportSettings& es, const Sequence& seq, bool enabled);

}
