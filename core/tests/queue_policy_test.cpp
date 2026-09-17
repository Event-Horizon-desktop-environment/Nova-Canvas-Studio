#include "canvas/core/export/queue_policy.hpp"

#include <cstdio>
#include <vector>

using namespace canvas::core;

namespace {

int failures = 0;

void check(const bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

void test_empty() {
    check(queue_policy::next_candidate({}) == -1, "empty -> -1");
    const std::vector<queue_policy::Candidate> none = {
        {0, 1, false}, {5, 2, false}};
    check(queue_policy::next_candidate(none) == -1, "all non-queued -> -1");
}

void test_fifo() {
    const std::vector<queue_policy::Candidate> jobs = {
        {0, 10, true}, {0, 11, true}, {0, 12, true}};
    check(queue_policy::next_candidate(jobs) == 0, "FIFO: earliest order first");

    const std::vector<queue_policy::Candidate> shuffled = {
        {0, 30, true}, {0, 10, true}, {0, 20, true}};
    check(queue_policy::next_candidate(shuffled) == 1, "FIFO picks smallest order");
}

void test_priority_inversion() {
    const std::vector<queue_policy::Candidate> jobs = {
        {0, 1, true},
        {5, 2, true},
        {1, 3, true}};
    check(queue_policy::next_candidate(jobs) == 1, "higher priority preempts earlier job");
}

void test_stable_tie() {
    const std::vector<queue_policy::Candidate> jobs = {
        {3, 7, true}, {3, 7, true}};
    check(queue_policy::next_candidate(jobs) == 0, "identical candidates -> lowest index");
}

void test_skips_non_queued() {
    const std::vector<queue_policy::Candidate> jobs = {
        {9, 1, false},
        {0, 2, true}};
    check(queue_policy::next_candidate(jobs) == 1, "non-queued high priority skipped");
}

}

int main() {
    test_empty();
    test_fifo();
    test_priority_inversion();
    test_stable_tie();
    test_skips_non_queued();

    if (failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "%d FAILURES\n", failures);
    return 1;
}
