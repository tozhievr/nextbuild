# Parallel builds and System Runtime

## Build graph

Nextbuild plans project dependencies from `Depend` and `UseDependency`. Within
each project it scans named module imports, including partitions, and waits for
the corresponding BMIs before compiling consumers. A project links or archives
after its compilation tasks finish. The scheduler stops starting new work if a
task fails, waits for work already running, and reports the failure.

```sh
nextbuild -j 8 --output stream --color auto apps/hello
nextbuild --graph apps/hello > build-graph.dot
dot -Tsvg build-graph.dot -o build-graph.svg
```

`-j N`, `-jN` and `--jobs N` set the build's concurrency budget, from 1 to 256.
The default uses the hardware concurrency up to 32. Independent projects
and independent compilation tasks can run concurrently within that budget.
`--graph` prints Graphviz DOT for the known project and module dependencies
without compiling. Header dependencies are read from compiler depfiles and
affect incremental rebuilds.

`--output on-demand` captures each tool's output. The CLI includes the
captured output when a command fails. `--output stream` writes stdout and
stderr to the corresponding terminal streams as bytes arrive. Output from
different processes can interleave. `--color auto` enables colors on a
terminal and respects `NO_COLOR`. Use `--color always` or `--color never`
for explicit control.

## System Runtime library

CMake consumers can link `Nextbuild::SystemRuntime` from an
`add_subdirectory` build. The named C++ module is `nextbuild.runtime`.

```cpp
#include <future>
#include <iostream>
#include <string_view>
import nextbuild.runtime;

int main() {
    using namespace nb::runtime;
    Pool runtime(8);
    auto capture = runtime.submit({"clang++", "--version"});
    auto streamed = runtime.submit({"clang++", "--version"}, {
        Mode::stream,
        [](Stream channel, std::string_view chunk) {
            auto& output = channel == Stream::stderr_stream ? std::cerr : std::cout;
            output << chunk << std::flush;
        }
    });
    const auto result = capture.get();
    std::cout << result.stdout_text;
    return result.exit_code ? static_cast<int>(result.exit_code)
                            : static_cast<int>(streamed.get().exit_code);
}
```

`Pool::submit` queues a process and returns a future. The pool can queue
hundreds of requests while limiting active subprocesses to its worker count.
Destroying the pool drains queued work and joins workers.
`execute(args, options)` runs the same process API synchronously.

The runtime passes arguments directly to the executable and closes the
child's stdin. On-demand mode captures stdout and stderr in separate strings
and returns them with the exit code. Stream mode requires a callback and
does not retain output in the result. The callback runs on the executing
worker. Its view is valid for the duration of the call. Calls for one child
are serialized, while callbacks from different children can run concurrently.
Chunks can split a line or a UTF-8 character. Launch and I/O errors are
reported as exceptions or through the future; a nonzero child exit code is
returned as data.
