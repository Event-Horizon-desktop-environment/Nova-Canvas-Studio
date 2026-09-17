#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace canvas::core::actions {

struct Action {
    std::string id;
    std::string title;
    std::vector<std::string> keywords;
    std::string shortcut;
    std::string category;
};

struct Hit {
    const Action* action = nullptr;
    int score = 0;
};

class Registry {
public:
    void add(Action action);

    [[nodiscard]] const std::vector<Action>& all() const noexcept { return actions_; }
    [[nodiscard]] std::size_t size() const noexcept { return actions_.size(); }

    [[nodiscard]] std::vector<Hit> search(std::string_view query, int limit = 12) const;

private:
    std::vector<Action> actions_;
};

[[nodiscard]] int score_action(const Action& action, std::string_view query);

}
