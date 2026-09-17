#pragma once

#include "canvas/core/project/project.hpp"

#include <string>
#include <vector>

namespace canvas::core::autosave {

struct Policy {
    int interval_seconds = 120;
    int max_snapshots = 5;
};

[[nodiscard]] std::string snapshot_path(const std::string& project_path, int slot);

[[nodiscard]] int slot_from_snapshot(const std::string& project_path, const std::string& path);

[[nodiscard]] int newest_slot(const std::vector<int>& existing_slots, const Policy& policy);

struct Turnover {
    int write_slot = 1;
    std::vector<int> remove_slots;
};

[[nodiscard]] Turnover plan_turnover(const std::vector<int>& existing_slots,
                                     const Policy& policy);

[[nodiscard]] std::string media_manifest(const Project& project);

}
