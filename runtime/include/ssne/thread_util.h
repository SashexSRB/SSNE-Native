// Detached host threads with large stacks. Guest calls nest as host calls, so guest recursion depth
// becomes host stack depth. Windows defaults to 1 MB, which isn't enough; reserve 64 MB (committed lazily).
#pragma once
#include <functional>

namespace ssne {
void spawn_guest_thread(std::function<void()> fn, size_t stack_bytes = size_t(64) << 20);
}
