#include "canvas/core/actions/action_registry.hpp"

#include <algorithm>
#include <cctype>

namespace canvas::core::actions {

namespace {

std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool starts_with(const std::string& hay, const std::string& needle) {
    return hay.size() >= needle.size() && hay.compare(0, needle.size(), needle) == 0;
}

}

int score_action(const Action& action, const std::string_view query) {
    const std::string q = lower(query);
    if (q.empty()) return 1;

    const std::string title = lower(action.title);
    if (title == q) return 1000;
    if (starts_with(title, q)) return 800;
    if (title.find(q) != std::string::npos) return 600;

    bool keyword_prefix = false;
    bool keyword_contains = false;
    for (const auto& k : action.keywords) {
        const std::string kl = lower(k);
        if (starts_with(kl, q)) keyword_prefix = true;
        else if (kl.find(q) != std::string::npos) keyword_contains = true;
    }
    if (keyword_prefix) return 400;
    if (keyword_contains) return 300;

    if (lower(action.category).find(q) != std::string::npos) return 150;
    return 0;
}

void Registry::add(Action action) {
    for (auto& existing : actions_) {
        if (existing.id == action.id) {
            existing = std::move(action);
            return;
        }
    }
    actions_.push_back(std::move(action));
}

std::vector<Hit> Registry::search(const std::string_view query, const int limit) const {
    std::vector<Hit> hits;
    for (const auto& a : actions_) {
        const int s = score_action(a, query);
        if (s > 0) hits.push_back(Hit{&a, s});
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& x, const Hit& y) { return x.score > y.score; });
    if (limit > 0 && hits.size() > static_cast<std::size_t>(limit))
        hits.resize(static_cast<std::size_t>(limit));
    return hits;
}

}
