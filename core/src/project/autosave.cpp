#include "canvas/core/project/autosave.hpp"

#include <algorithm>

namespace canvas::core::autosave {

namespace {

std::string suffix() { return ".autosave-"; }

int clamp_max(const int max_snapshots) { return std::max(1, max_snapshots); }

}

std::string snapshot_path(const std::string& project_path, const int slot) {
    return project_path + suffix() + std::to_string(std::max(1, slot));
}

int slot_from_snapshot(const std::string& project_path, const std::string& path) {
    const std::string prefix = project_path + suffix();
    if (path.size() <= prefix.size() || path.compare(0, prefix.size(), prefix) != 0) return 0;
    const std::string tail = path.substr(prefix.size());
    if (tail.empty()) return 0;
    int slot = 0;
    for (const char c : tail) {
        if (c < '0' || c > '9') return 0;
        slot = slot * 10 + (c - '0');
        if (slot > 1000000) return 0;
    }
    return slot >= 1 ? slot : 0;
}

int newest_slot(const std::vector<int>& existing_slots, const Policy& policy) {
    const int max_slot = clamp_max(policy.max_snapshots);
    int newest = 0;
    for (const int s : existing_slots)
        if (s >= 1 && s <= max_slot) newest = std::max(newest, s);
    return newest;
}

Turnover plan_turnover(const std::vector<int>& existing_slots, const Policy& policy) {
    const int max_slot = clamp_max(policy.max_snapshots);

    std::vector<int> valid;
    Turnover out;
    for (const int s : existing_slots) {
        if (s >= 1 && s <= max_slot) {
            if (std::find(valid.begin(), valid.end(), s) == valid.end()) valid.push_back(s);
        } else if (std::find(out.remove_slots.begin(), out.remove_slots.end(), s) ==
                   out.remove_slots.end()) {
            if (s >= 1) out.remove_slots.push_back(s);
        }
    }

    int slot = 0;
    for (int s = 1; s <= max_slot; ++s)
        if (std::find(valid.begin(), valid.end(), s) == valid.end()) {
            slot = s;
            break;
        }
    if (slot == 0) {
        const int newest = newest_slot(valid, policy);
        slot = (newest % max_slot) + 1;
    }
    out.write_slot = slot;
    return out;
}

std::string media_manifest(const Project& project) {
    std::string out = "Project: " + project.name + "\n";
    out += "Media: " + std::to_string(project.media.size()) + "\n";
    for (const auto& m : project.media) {
        out += "  [" + std::to_string(m.id) + "] " + std::to_string(m.width) + "x" +
               std::to_string(m.height) + " @ " + m.path + "\n";
    }
    return out;
}

}
