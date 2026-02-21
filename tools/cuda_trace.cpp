#include <cuda.h>
#include <dlfcn.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

namespace {

using steady_clock_t = std::chrono::steady_clock;
using vt_device_h = void *;

static bool trace_enabled() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;
  const char *v = std::getenv("CUDA_TRACE");
  enabled = (v && *v && std::string(v) != "0") ? 1 : 0;
  return enabled != 0;
}

static bool trace_resolve_enabled() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;
  const char *v = std::getenv("CUDA_TRACE_RESOLVE");
  enabled = (v && *v && std::string(v) != "0") ? 1 : 0;
  return enabled != 0;
}

static unsigned long long pid() { return static_cast<unsigned long long>(::getpid()); }

static unsigned long long tid() {
  static_assert(sizeof(std::thread::id) <= sizeof(unsigned long long));
  unsigned long long out = 0;
  const auto id = std::this_thread::get_id();
  std::memcpy(&out, &id, sizeof(id));
  return out;
}

static double ms_since(steady_clock_t::time_point a, steady_clock_t::time_point b) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(b - a).count();
}

static void log_resolve(const char *name, void *p) {
  if (!trace_resolve_enabled()) return;
  if (!p) {
    std::fprintf(stderr, "[cuda-trace pid=%llu tid=%llu] resolve %s -> <null>\n", pid(), tid(), name);
    return;
  }
  Dl_info info{};
  const int ok = dladdr(p, &info);
  if (!ok) {
    std::fprintf(stderr, "[cuda-trace pid=%llu tid=%llu] resolve %s -> %p (dladdr failed)\n", pid(), tid(), name, p);
    return;
  }
  std::fprintf(stderr, "[cuda-trace pid=%llu tid=%llu] resolve %s -> %p (%s)\n", pid(), tid(), name, p, (info.dli_fname ? info.dli_fname : "?"));
}

template <class Fn>
static Fn load_sym(const char *name) {
  void *p = dlsym(RTLD_NEXT, name);
  if (!p) {
    // Prefer an already-loaded libcuda to avoid mixing handles (CUmodule/CUfunction are not portable across copies).
    static void *cuda_handle = nullptr;
    static std::once_flag once;
    std::call_once(once, [&]() {
      cuda_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
      if (!cuda_handle) {
        // As a last resort, load the real driver library by SONAME. Avoid libcuda.so (toolkit stubs).
        cuda_handle = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
      }
    });
    if (cuda_handle) p = dlsym(cuda_handle, name);
  }
  log_resolve(name, p);
  return reinterpret_cast<Fn>(p);
}

template <class Fn>
static Fn load_sym_vt(const char *name) {
  void *p = dlsym(RTLD_NEXT, name);
  if (!p) {
    // PoCL loads the Ventus driver backend as a shared object; it may be RTLD_LOCAL.
    // Prefer an already-loaded backend library (NOLOAD) to avoid creating a second copy.
    static void *handles[3] = {nullptr, nullptr, nullptr};
    static std::once_flag once;
    std::call_once(once, [&]() {
      handles[0] = dlopen("libptx_driver.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
      handles[1] = dlopen("libventus_driver.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
      handles[2] = dlopen("libauto_select_driver.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    });
    for (void *h : handles) {
      if (!h) continue;
      p = dlsym(h, name);
      if (p) break;
    }
  }
  log_resolve(name, p);
  return reinterpret_cast<Fn>(p);
}

struct State final {
  std::mutex mu;
  std::unordered_map<CUfunction, std::string> func_name;
  steady_clock_t::time_point last_launch = steady_clock_t::time_point{};
  CUfunction last_fun{};
  unsigned last_grid[3]{};
  unsigned last_block[3]{};
  unsigned last_shmem = 0;

  unsigned long long n_launch = 0;
  unsigned long long n_sync = 0;
  double launch_call_ms = 0.0;
  double sync_call_ms = 0.0;
  double sync_since_launch_ms = 0.0; // sum of (sync_end - last_launch_end)

  unsigned long long n_module_load = 0;
  double module_load_ms = 0.0;

  unsigned long long n_mem_alloc = 0;
  unsigned long long mem_alloc_bytes = 0;
  double mem_alloc_ms = 0.0;

  unsigned long long n_mem_free = 0;
  double mem_free_ms = 0.0;

  unsigned long long n_memset_d8 = 0;
  unsigned long long memset_d8_bytes = 0;
  double memset_d8_ms = 0.0;

  unsigned long long n_memcpy_htod = 0;
  unsigned long long memcpy_htod_bytes = 0;
  double memcpy_htod_ms = 0.0;

  unsigned long long n_memcpy_dtoh = 0;
  unsigned long long memcpy_dtoh_bytes = 0;
  double memcpy_dtoh_ms = 0.0;

  unsigned long long n_vt_start = 0;
  double vt_start_ms = 0.0;

  unsigned long long n_vt_ready_wait = 0;
  double vt_ready_wait_ms = 0.0;

  unsigned long long n_vt_buf_alloc = 0;
  unsigned long long vt_buf_alloc_bytes = 0;
  double vt_buf_alloc_ms = 0.0;

  unsigned long long n_vt_copy_to_dev = 0;
  unsigned long long vt_copy_to_dev_bytes = 0;
  double vt_copy_to_dev_ms = 0.0;

  unsigned long long n_vt_copy_from_dev = 0;
  unsigned long long vt_copy_from_dev_bytes = 0;
  double vt_copy_from_dev_ms = 0.0;
};

static State &S() {
  static State s;
  return s;
}

static CUresult (*real_cuModuleGetFunction)(CUfunction *, CUmodule, const char *) = nullptr;
static CUresult (*real_cuModuleLoadDataEx)(CUmodule *, const void *, unsigned int, CUjit_option *, void **) = nullptr;
static CUresult (*real_cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream, void **, void **) = nullptr;
static CUresult (*real_cuCtxSynchronize)(void) = nullptr;
static CUresult (*real_cuMemAlloc_v2)(CUdeviceptr *, size_t) = nullptr;
static CUresult (*real_cuMemFree_v2)(CUdeviceptr) = nullptr;
static CUresult (*real_cuMemsetD8_v2)(CUdeviceptr, unsigned char, size_t) = nullptr;
static CUresult (*real_cuMemsetD8_v2_ptds)(CUdeviceptr, unsigned char, size_t) = nullptr;
static CUresult (*real_cuMemcpyHtoD_v2)(CUdeviceptr, const void *, size_t) = nullptr;
static CUresult (*real_cuMemcpyHtoD_v2_ptds)(CUdeviceptr, const void *, size_t) = nullptr;
static CUresult (*real_cuMemcpyDtoH_v2)(void *, CUdeviceptr, size_t) = nullptr;
static CUresult (*real_cuMemcpyDtoH_v2_ptds)(void *, CUdeviceptr, size_t) = nullptr;
static CUresult (*real_cuLaunchKernel_ptsz)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUstream, void **, void **) = nullptr;

static int (*real_vt_start)(vt_device_h, void *, unsigned long long) = nullptr;
static int (*real_vt_ready_wait)(vt_device_h, unsigned long long) = nullptr;
static int (*real_vt_buf_alloc)(vt_device_h, unsigned long long, unsigned long long *, int, unsigned long long, unsigned long long) = nullptr;
static int (*real_vt_copy_to_dev)(vt_device_h, unsigned long long, const void *, unsigned long long, unsigned long long, unsigned long long) = nullptr;
static int (*real_vt_copy_from_dev)(vt_device_h, unsigned long long, void *, unsigned long long, unsigned long long, unsigned long long) = nullptr;

static void ensure_init() {
  static std::once_flag once;
  std::call_once(once, []() {
    real_cuModuleGetFunction = load_sym<decltype(real_cuModuleGetFunction)>("cuModuleGetFunction");
    real_cuModuleLoadDataEx = load_sym<decltype(real_cuModuleLoadDataEx)>("cuModuleLoadDataEx");
    real_cuLaunchKernel = load_sym<decltype(real_cuLaunchKernel)>("cuLaunchKernel");
    real_cuLaunchKernel_ptsz = load_sym<decltype(real_cuLaunchKernel_ptsz)>("cuLaunchKernel_ptsz");
    real_cuCtxSynchronize = load_sym<decltype(real_cuCtxSynchronize)>("cuCtxSynchronize");
    real_cuMemAlloc_v2 = load_sym<decltype(real_cuMemAlloc_v2)>("cuMemAlloc_v2");
    real_cuMemFree_v2 = load_sym<decltype(real_cuMemFree_v2)>("cuMemFree_v2");
    real_cuMemsetD8_v2 = load_sym<decltype(real_cuMemsetD8_v2)>("cuMemsetD8_v2");
    real_cuMemsetD8_v2_ptds = load_sym<decltype(real_cuMemsetD8_v2_ptds)>("cuMemsetD8_v2_ptds");
    real_cuMemcpyHtoD_v2 = load_sym<decltype(real_cuMemcpyHtoD_v2)>("cuMemcpyHtoD_v2");
    real_cuMemcpyHtoD_v2_ptds = load_sym<decltype(real_cuMemcpyHtoD_v2_ptds)>("cuMemcpyHtoD_v2_ptds");
    real_cuMemcpyDtoH_v2 = load_sym<decltype(real_cuMemcpyDtoH_v2)>("cuMemcpyDtoH_v2");
    real_cuMemcpyDtoH_v2_ptds = load_sym<decltype(real_cuMemcpyDtoH_v2_ptds)>("cuMemcpyDtoH_v2_ptds");
  });
}

static void ensure_vt_init() {
  static std::once_flag once;
  std::call_once(once, []() {
    real_vt_start = load_sym_vt<decltype(real_vt_start)>("vt_start");
    real_vt_ready_wait = load_sym_vt<decltype(real_vt_ready_wait)>("vt_ready_wait");
    real_vt_buf_alloc = load_sym_vt<decltype(real_vt_buf_alloc)>("vt_buf_alloc");
    real_vt_copy_to_dev = load_sym_vt<decltype(real_vt_copy_to_dev)>("vt_copy_to_dev");
    real_vt_copy_from_dev = load_sym_vt<decltype(real_vt_copy_from_dev)>("vt_copy_from_dev");
  });
}

static std::string lookup_name(CUfunction f) {
  auto &st = S();
  std::lock_guard<std::mutex> lock(st.mu);
  auto it = st.func_name.find(f);
  if (it == st.func_name.end()) return "<unknown>";
  return it->second;
}

static bool trace_per_call() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;
  const char *v = std::getenv("CUDA_TRACE_PER_CALL");
  enabled = (v && *v && std::string(v) != "0") ? 1 : 0;
  return enabled != 0;
}

static bool trace_summary() {
  static int enabled = -1;
  if (enabled != -1) return enabled != 0;
  const char *v = std::getenv("CUDA_TRACE_SUMMARY");
  enabled = (v && *v && std::string(v) != "0") ? 1 : 0;
  return enabled != 0;
}

static void dump_summary() {
  if (!trace_enabled()) return;
  if (!trace_summary()) return;
  auto &st = S();
  std::lock_guard<std::mutex> lock(st.mu);
  if (st.n_launch == 0 && st.n_sync == 0 && st.n_mem_alloc == 0 && st.n_mem_free == 0 && st.n_memset_d8 == 0 && st.n_memcpy_htod == 0 &&
      st.n_memcpy_dtoh == 0 && st.n_module_load == 0 && st.n_vt_start == 0 && st.n_vt_ready_wait == 0 && st.n_vt_buf_alloc == 0 &&
      st.n_vt_copy_to_dev == 0 && st.n_vt_copy_from_dev == 0) {
    return;
  }
  const double avg_launch = (st.n_launch ? (st.launch_call_ms / (double)st.n_launch) : 0.0);
  const double avg_sync = (st.n_sync ? (st.sync_call_ms / (double)st.n_sync) : 0.0);
  const double avg_since_launch = (st.n_sync ? (st.sync_since_launch_ms / (double)st.n_sync) : 0.0);
  std::fprintf(stderr,
               "[cuda-trace pid=%llu] summary: launches=%llu launch_call=%.3fms avg=%.6fms syncs=%llu sync_call=%.3fms avg=%.6fms sync_since_launch=%.3fms avg=%.6fms "
               "module_loads=%llu module_load=%.3fms mem_alloc=%llu bytes=%llu ms=%.3f mem_free=%llu ms=%.3f memset_d8=%llu bytes=%llu ms=%.3f "
               "memcpy_htod=%llu bytes=%llu ms=%.3f memcpy_dtoh=%llu bytes=%llu ms=%.3f "
               "vt_start=%llu ms=%.3f vt_ready_wait=%llu ms=%.3f vt_buf_alloc=%llu bytes=%llu ms=%.3f vt_copy_to_dev=%llu bytes=%llu ms=%.3f vt_copy_from_dev=%llu bytes=%llu ms=%.3f\n",
               pid(), st.n_launch, st.launch_call_ms, avg_launch, st.n_sync, st.sync_call_ms, avg_sync, st.sync_since_launch_ms, avg_since_launch,
               st.n_module_load, st.module_load_ms, st.n_mem_alloc, st.mem_alloc_bytes, st.mem_alloc_ms, st.n_mem_free, st.mem_free_ms,
               st.n_memset_d8, st.memset_d8_bytes, st.memset_d8_ms, st.n_memcpy_htod, st.memcpy_htod_bytes, st.memcpy_htod_ms, st.n_memcpy_dtoh,
               st.memcpy_dtoh_bytes, st.memcpy_dtoh_ms, st.n_vt_start, st.vt_start_ms, st.n_vt_ready_wait, st.vt_ready_wait_ms, st.n_vt_buf_alloc,
               st.vt_buf_alloc_bytes, st.vt_buf_alloc_ms, st.n_vt_copy_to_dev, st.vt_copy_to_dev_bytes, st.vt_copy_to_dev_ms, st.n_vt_copy_from_dev,
               st.vt_copy_from_dev_bytes, st.vt_copy_from_dev_ms);
}

} // namespace

__attribute__((destructor)) static void cuda_trace_destructor() { dump_summary(); }

extern "C" CUresult CUDAAPI cuModuleLoadDataEx(CUmodule *module, const void *image, unsigned int numOptions, CUjit_option *options, void **optionValues) {
  ensure_init();
  if (!real_cuModuleLoadDataEx) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return real_cuModuleLoadDataEx(module, image, numOptions, options, optionValues);
  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuModuleLoadDataEx(module, image, numOptions, options, optionValues);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_module_load++;
    st.module_load_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuModuleGetFunction(CUfunction *hfunc, CUmodule hmod, const char *name) {
  ensure_init();
  if (!real_cuModuleGetFunction) return CUDA_ERROR_UNKNOWN;
  const CUresult r = real_cuModuleGetFunction(hfunc, hmod, name);
  if (trace_enabled() && r == CUDA_SUCCESS && hfunc && *hfunc && name) {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.func_name[*hfunc] = name;
  }
  return r;
}

extern "C" CUresult CUDAAPI cuLaunchKernel(CUfunction f, unsigned gridDimX, unsigned gridDimY, unsigned gridDimZ, unsigned blockDimX, unsigned blockDimY,
                                          unsigned blockDimZ, unsigned sharedMemBytes, CUstream hStream, void **kernelParams, void **extra) {
  (void)hStream;
  (void)kernelParams;
  (void)extra;

  ensure_init();
  if (!real_cuLaunchKernel) return CUDA_ERROR_UNKNOWN;
  const bool trace = trace_enabled();
  if (!trace) return real_cuLaunchKernel(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream, kernelParams, extra);

  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuLaunchKernel(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
  const auto t1 = steady_clock_t::now();

  if (trace) {
    const double call_ms = ms_since(t0, t1);
    {
      auto &st = S();
      std::lock_guard<std::mutex> lock(st.mu);
      st.n_launch++;
      st.launch_call_ms += call_ms;
      st.last_launch = t1;
      st.last_fun = f;
      st.last_grid[0] = gridDimX;
      st.last_grid[1] = gridDimY;
      st.last_grid[2] = gridDimZ;
      st.last_block[0] = blockDimX;
      st.last_block[1] = blockDimY;
      st.last_block[2] = blockDimZ;
      st.last_shmem = sharedMemBytes;
    }
    if (trace_per_call()) {
      const std::string name = lookup_name(f);
      std::fprintf(stderr,
                   "[cuda-trace pid=%llu tid=%llu] cuLaunchKernel %s grid=%ux%ux%u block=%ux%ux%u shmem=%u rc=%d %.3fms\n", pid(), tid(),
                   name.c_str(), gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, (int)r, call_ms);
    }
  }

  return r;
}

extern "C" CUresult CUDAAPI cuLaunchKernel_ptsz(CUfunction f, unsigned gridDimX, unsigned gridDimY, unsigned gridDimZ, unsigned blockDimX, unsigned blockDimY,
                                               unsigned blockDimZ, unsigned sharedMemBytes, CUstream hStream, void **kernelParams, void **extra) {
  ensure_init();
  if (!real_cuLaunchKernel_ptsz) return CUDA_ERROR_UNKNOWN;
  const bool trace = trace_enabled();
  if (!trace) return real_cuLaunchKernel_ptsz(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream, kernelParams, extra);

  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuLaunchKernel_ptsz(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
  const auto t1 = steady_clock_t::now();

  const double call_ms = ms_since(t0, t1);
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_launch++;
    st.launch_call_ms += call_ms;
    st.last_launch = t1;
    st.last_fun = f;
    st.last_grid[0] = gridDimX;
    st.last_grid[1] = gridDimY;
    st.last_grid[2] = gridDimZ;
    st.last_block[0] = blockDimX;
    st.last_block[1] = blockDimY;
    st.last_block[2] = blockDimZ;
    st.last_shmem = sharedMemBytes;
  }
  if (trace_per_call()) {
    const std::string name = lookup_name(f);
    std::fprintf(stderr,
                 "[cuda-trace pid=%llu tid=%llu] cuLaunchKernel_ptsz %s grid=%ux%ux%u block=%ux%ux%u shmem=%u rc=%d %.3fms\n", pid(), tid(),
                 name.c_str(), gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, (int)r, call_ms);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuCtxSynchronize(void) {
  ensure_init();
  if (!real_cuCtxSynchronize) return CUDA_ERROR_UNKNOWN;
  const bool trace = trace_enabled();
  if (!trace) return real_cuCtxSynchronize();

  steady_clock_t::time_point last_launch_tp{};
  CUfunction last_fun{};
  unsigned g[3]{}, b[3]{}, sh = 0;
  if (trace) {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    last_launch_tp = st.last_launch;
    last_fun = st.last_fun;
    g[0] = st.last_grid[0];
    g[1] = st.last_grid[1];
    g[2] = st.last_grid[2];
    b[0] = st.last_block[0];
    b[1] = st.last_block[1];
    b[2] = st.last_block[2];
    sh = st.last_shmem;
  }

  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuCtxSynchronize();
  const auto t1 = steady_clock_t::now();

  if (trace) {
    const double call_ms = ms_since(t0, t1);
    double since_launch_ms = -1.0;
    if (last_launch_tp != steady_clock_t::time_point{}) since_launch_ms = ms_since(last_launch_tp, t1);
    {
      auto &st = S();
      std::lock_guard<std::mutex> lock(st.mu);
      st.n_sync++;
      st.sync_call_ms += call_ms;
      if (since_launch_ms >= 0.0) st.sync_since_launch_ms += since_launch_ms;
    }
    if (trace_per_call()) {
      const std::string name = (last_fun ? lookup_name(last_fun) : std::string("<none>"));
      std::fprintf(stderr,
                   "[cuda-trace pid=%llu tid=%llu] cuCtxSynchronize rc=%d call=%.3fms since_launch=%.3fms last=%s grid=%ux%ux%u block=%ux%ux%u shmem=%u\n",
                   pid(), tid(), (int)r, call_ms, since_launch_ms, name.c_str(), g[0], g[1], g[2], b[0], b[1], b[2], sh);
    }
  }

  return r;
}

extern "C" CUresult CUDAAPI cuMemAlloc_v2(CUdeviceptr *dptr, size_t bytesize) {
  ensure_init();
  if (!real_cuMemAlloc_v2) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return real_cuMemAlloc_v2(dptr, bytesize);
  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuMemAlloc_v2(dptr, bytesize);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_mem_alloc++;
    st.mem_alloc_bytes += static_cast<unsigned long long>(bytesize);
    st.mem_alloc_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuMemFree_v2(CUdeviceptr dptr) {
  ensure_init();
  if (!real_cuMemFree_v2) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return real_cuMemFree_v2(dptr);
  const auto t0 = steady_clock_t::now();
  const CUresult r = real_cuMemFree_v2(dptr);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_mem_free++;
    st.mem_free_ms += ms_since(t0, t1);
  }
  return r;
}

static CUresult do_memset_d8(CUdeviceptr dstDevice, unsigned char uc, size_t N, CUresult (*fn)(CUdeviceptr, unsigned char, size_t)) {
  if (!fn) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return fn(dstDevice, uc, N);
  const auto t0 = steady_clock_t::now();
  const CUresult r = fn(dstDevice, uc, N);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_memset_d8++;
    st.memset_d8_bytes += static_cast<unsigned long long>(N);
    st.memset_d8_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuMemsetD8_v2(CUdeviceptr dstDevice, unsigned char uc, size_t N) {
  ensure_init();
  return do_memset_d8(dstDevice, uc, N, real_cuMemsetD8_v2);
}

extern "C" CUresult CUDAAPI cuMemsetD8_v2_ptds(CUdeviceptr dstDevice, unsigned char uc, size_t N) {
  ensure_init();
  return do_memset_d8(dstDevice, uc, N, real_cuMemsetD8_v2_ptds);
}

static CUresult do_memcpy_htod(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount, CUresult (*fn)(CUdeviceptr, const void *, size_t)) {
  if (!fn) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return fn(dstDevice, srcHost, ByteCount);
  const auto t0 = steady_clock_t::now();
  const CUresult r = fn(dstDevice, srcHost, ByteCount);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_memcpy_htod++;
    st.memcpy_htod_bytes += static_cast<unsigned long long>(ByteCount);
    st.memcpy_htod_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuMemcpyHtoD_v2(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount) {
  ensure_init();
  return do_memcpy_htod(dstDevice, srcHost, ByteCount, real_cuMemcpyHtoD_v2);
}

extern "C" CUresult CUDAAPI cuMemcpyHtoD_v2_ptds(CUdeviceptr dstDevice, const void *srcHost, size_t ByteCount) {
  ensure_init();
  return do_memcpy_htod(dstDevice, srcHost, ByteCount, real_cuMemcpyHtoD_v2_ptds);
}

static CUresult do_memcpy_dtoh(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount, CUresult (*fn)(void *, CUdeviceptr, size_t)) {
  if (!fn) return CUDA_ERROR_UNKNOWN;
  if (!trace_enabled()) return fn(dstHost, srcDevice, ByteCount);
  const auto t0 = steady_clock_t::now();
  const CUresult r = fn(dstHost, srcDevice, ByteCount);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_memcpy_dtoh++;
    st.memcpy_dtoh_bytes += static_cast<unsigned long long>(ByteCount);
    st.memcpy_dtoh_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" CUresult CUDAAPI cuMemcpyDtoH_v2(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount) {
  ensure_init();
  return do_memcpy_dtoh(dstHost, srcDevice, ByteCount, real_cuMemcpyDtoH_v2);
}

extern "C" CUresult CUDAAPI cuMemcpyDtoH_v2_ptds(void *dstHost, CUdeviceptr srcDevice, size_t ByteCount) {
  ensure_init();
  return do_memcpy_dtoh(dstHost, srcDevice, ByteCount, real_cuMemcpyDtoH_v2_ptds);
}

extern "C" int vt_start(vt_device_h hdevice, void *metaData, unsigned long long taskID) {
  ensure_vt_init();
  if (!real_vt_start) return -1;
  if (!trace_enabled()) return real_vt_start(hdevice, metaData, taskID);
  const auto t0 = steady_clock_t::now();
  const int r = real_vt_start(hdevice, metaData, taskID);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_vt_start++;
    st.vt_start_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" int vt_ready_wait(vt_device_h hdevice, unsigned long long timeout) {
  ensure_vt_init();
  if (!real_vt_ready_wait) return -1;
  if (!trace_enabled()) return real_vt_ready_wait(hdevice, timeout);
  const auto t0 = steady_clock_t::now();
  const int r = real_vt_ready_wait(hdevice, timeout);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_vt_ready_wait++;
    st.vt_ready_wait_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" int vt_buf_alloc(vt_device_h hdevice, unsigned long long size, unsigned long long *vaddr, int BUF_TYPE, unsigned long long taskID,
                            unsigned long long kernelID) {
  ensure_vt_init();
  if (!real_vt_buf_alloc) return -1;
  if (!trace_enabled()) return real_vt_buf_alloc(hdevice, size, vaddr, BUF_TYPE, taskID, kernelID);
  const auto t0 = steady_clock_t::now();
  const int r = real_vt_buf_alloc(hdevice, size, vaddr, BUF_TYPE, taskID, kernelID);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_vt_buf_alloc++;
    st.vt_buf_alloc_bytes += size;
    st.vt_buf_alloc_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" int vt_copy_to_dev(vt_device_h hdevice, unsigned long long dev_vaddr, const void *src_addr, unsigned long long size,
                              unsigned long long taskID, unsigned long long kernelID) {
  ensure_vt_init();
  if (!real_vt_copy_to_dev) return -1;
  if (!trace_enabled()) return real_vt_copy_to_dev(hdevice, dev_vaddr, src_addr, size, taskID, kernelID);
  const auto t0 = steady_clock_t::now();
  const int r = real_vt_copy_to_dev(hdevice, dev_vaddr, src_addr, size, taskID, kernelID);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_vt_copy_to_dev++;
    st.vt_copy_to_dev_bytes += size;
    st.vt_copy_to_dev_ms += ms_since(t0, t1);
  }
  return r;
}

extern "C" int vt_copy_from_dev(vt_device_h hdevice, unsigned long long dev_vaddr, void *dst_addr, unsigned long long size,
                                unsigned long long taskID, unsigned long long kernelID) {
  ensure_vt_init();
  if (!real_vt_copy_from_dev) return -1;
  if (!trace_enabled()) return real_vt_copy_from_dev(hdevice, dev_vaddr, dst_addr, size, taskID, kernelID);
  const auto t0 = steady_clock_t::now();
  const int r = real_vt_copy_from_dev(hdevice, dev_vaddr, dst_addr, size, taskID, kernelID);
  const auto t1 = steady_clock_t::now();
  {
    auto &st = S();
    std::lock_guard<std::mutex> lock(st.mu);
    st.n_vt_copy_from_dev++;
    st.vt_copy_from_dev_bytes += size;
    st.vt_copy_from_dev_ms += ms_since(t0, t1);
  }
  return r;
}
