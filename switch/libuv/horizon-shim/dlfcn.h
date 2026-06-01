/* dlfcn.h stub for Nintendo Switch: no dynamic loading. libuv's uv_dlopen maps
   onto these; they fail so uv_dlopen reports an error (correct on Switch). */
#ifndef NX_DLFCN_H_
#define NX_DLFCN_H_
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY   1
#define RTLD_NOW    2
#define RTLD_GLOBAL 4
#define RTLD_LOCAL  8
static inline void* dlopen(const char* f, int m) { (void)f; (void)m; return 0; }
static inline int   dlclose(void* h) { (void)h; return 0; }
static inline void* dlsym(void* h, const char* s) { (void)h; (void)s; return 0; }
static inline char* dlerror(void) { return (char*)"dlopen unsupported on Horizon"; }
#ifdef __cplusplus
}
#endif
#endif
