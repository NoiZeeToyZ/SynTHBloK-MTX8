#pragma once
// One object per Rack process, shared by every SynTHBloK plugin. Each plugin is its own .dll / .so / .dylib with its
// own copy of every static, so a plain function-local static gives each plugin its own copy: the Composer in MTX
// Brains would not see the MTX Grooves modules, and two plugins would each think they own the JAM. The first plugin
// to ask makes the object and leaves its address in an environment variable of this Rack process; the others pick
// it up. Never freed (it lives until Rack quits). Bump the number in a key whenever its T changes layout, so plugins
// built with different layouts never share one. Ask for it first at plugin load (see SHAREDSLOT_WARM), on Rack's
// main thread, so two plugins never race to make it.
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char* name, char* buf, unsigned long size);
extern "C" __declspec(dllimport) int __stdcall SetEnvironmentVariableA(const char* name, const char* value);
#endif

namespace sharedslot {

inline void* find(const char* key) {
    char buf[40] = {0};
#ifdef _WIN32
    unsigned long n = GetEnvironmentVariableA(key, buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return NULL;
#else
    const char* s = std::getenv(key);
    if (!s) return NULL;
    std::snprintf(buf, sizeof buf, "%s", s);
#endif
    void* p = NULL;
    return std::sscanf(buf, "%p", &p) == 1 ? p : NULL;
}

inline void publish(const char* key, void* p) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%p", p);
#ifdef _WIN32
    SetEnvironmentVariableA(key, buf);
#else
    setenv(key, buf, 1);
#endif
}

template <class T> T* get(const char* key) {
    if (void* p = find(key)) return (T*) p;
    T* t = new T();
    publish(key, t);
    return t;
}

}  // namespace sharedslot

// makes the shared object when the plugin loads (static init runs on Rack's main thread, one plugin at a time)
#define SHAREDSLOT_WARM(name, expr) namespace { __attribute__((unused)) const bool name = ((void) (expr), true); }
