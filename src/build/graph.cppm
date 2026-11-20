module;
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

export module nextbuild.graph;

export namespace nb::graph {
using Edges = std::vector<std::vector<std::size_t>>;
std::size_t maximum_layer(const Edges& dependencies);
void execute(const Edges& dependencies, unsigned jobs, const std::function<void(std::size_t)>& action);
}

namespace nb::graph {
namespace {
struct State {
    Edges dependents;
    std::vector<std::size_t> remaining;
    std::deque<std::size_t> ready;

    explicit State(const Edges& edges) : dependents(edges.size()), remaining(edges.size()) {
        for (std::size_t i = 0; i < edges.size(); ++i) {
            for (const auto dependency : edges[i]) {
                if (dependency >= edges.size()) throw std::logic_error("invalid build graph node");
                dependents[dependency].push_back(i);
            }
            remaining[i] = edges[i].size();
            if (!remaining[i]) ready.push_back(i);
        }
    }

    void complete(std::size_t node) {
        for (const auto other : dependents[node])
            if (--remaining[other] == 0) ready.push_back(other);
    }
};
}

std::size_t maximum_layer(const Edges& dependencies) {
    State state(dependencies);
    std::size_t maximum = 0, visited = 0;
    while (!state.ready.empty()) {
        const auto width = state.ready.size();
        visited += width;
        maximum = std::max(maximum, width);
        for (std::size_t i = 0; i < width; ++i) {
            const auto node = state.ready.front();
            state.ready.pop_front();
            state.complete(node);
        }
    }
    if (visited != dependencies.size()) throw std::runtime_error("cycle in build graph");
    return maximum;
}

void execute(const Edges& dependencies, unsigned jobs, const std::function<void(std::size_t)>& action) {
    if (!jobs) throw std::invalid_argument("build job count must be positive");
    const auto count = std::min<std::size_t>(jobs, maximum_layer(dependencies));
    if (!count) return;
    State state(dependencies);
    std::mutex mutex;
    std::condition_variable wake;
    std::exception_ptr failure;
    std::size_t completed = 0;
    const auto worker = [&] {
        for (;;) {
            std::size_t node;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] {
                    return failure || !state.ready.empty() || completed == dependencies.size();
                });
                if (failure || completed == dependencies.size()) return;
                node = state.ready.front();
                state.ready.pop_front();
            }
            try {
                action(node);
                {
                    std::scoped_lock lock(mutex);
                    ++completed;
                    state.complete(node);
                }
            } catch (...) {
                std::scoped_lock lock(mutex);
                if (!failure) failure = std::current_exception();
            }
            wake.notify_all();
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(count - 1);
    try {
        for (std::size_t i = 1; i < count; ++i) workers.emplace_back(worker);
    } catch (...) {
        {
            std::scoped_lock lock(mutex);
            failure = std::current_exception();
        }
        wake.notify_all();
    }
    worker();
    for (auto& thread : workers) thread.join();
    if (failure) std::rethrow_exception(failure);
}
}
