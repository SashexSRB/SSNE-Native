#include "ssne/thread_util.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ssne {

namespace {
#ifdef _WIN32
DWORD WINAPI trampoline(LPVOID p) {
#else
void* trampoline(void* p) {
#endif
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(p));
    (*fn)();
    return 0;
}
} // namespace

void spawn_guest_thread(std::function<void()> fn, size_t stack_bytes) {
    auto* heap_fn = new std::function<void()>(std::move(fn));
#ifdef _WIN32
    HANDLE h = CreateThread(nullptr, stack_bytes, trampoline, heap_fn, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { fprintf(stderr, "[ssne] CreateThread failed\n"); std::abort(); }
    CloseHandle(h);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, trampoline, heap_fn) != 0) { fprintf(stderr, "[ssne] pthread_create failed\n"); std::abort(); }
    pthread_attr_destroy(&attr);
#endif
}

} // namespace ssne
