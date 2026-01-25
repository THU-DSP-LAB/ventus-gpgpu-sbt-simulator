#include <cuda.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CUCHK(expr)                                                     \
  do {                                                                  \
    CUresult _res = (expr);                                             \
    if (_res != CUDA_SUCCESS) {                                         \
      const char *name = nullptr;                                       \
      const char *desc = nullptr;                                       \
      cuGetErrorName(_res, &name);                                      \
      cuGetErrorString(_res, &desc);                                    \
      std::fprintf(stderr, "CUDA driver error %d (%s): %s\n",           \
                   (int)_res, name ? name : "?", desc ? desc : "?"); \
      std::exit(1);                                                     \
    }                                                                   \
  } while (0)

static std::vector<char> read_file(const char *path) {
  std::FILE *f = std::fopen(path, "rb");
  if (!f) {
    std::perror(path);
    std::exit(1);
  }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    std::fprintf(stderr, "empty file: %s\n", path);
    std::exit(1);
  }
  std::vector<char> buf((size_t)n);
  if (std::fread(buf.data(), 1, (size_t)n, f) != (size_t)n) {
    std::fprintf(stderr, "failed to read: %s\n", path);
    std::exit(1);
  }
  std::fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  const char *cubin_path = (argc >= 2) ? argv[1] : "build/simple.cubin";
  const char *kernel_name = (argc >= 3) ? argv[2] : "vecadd_simple";

  CUCHK(cuInit(0));

  CUdevice dev;
  CUCHK(cuDeviceGet(&dev, 0));

  // Use the device primary context to avoid cuCtxCreate() version churn across CUDA toolkits.
  CUcontext ctx;
  CUCHK(cuDevicePrimaryCtxRetain(&ctx, dev));
  CUCHK(cuCtxSetCurrent(ctx));

  auto cubin = read_file(cubin_path);

  CUmodule mod;
  CUCHK(cuModuleLoadData(&mod, cubin.data()));

  CUfunction fun;
  CUCHK(cuModuleGetFunction(&fun, mod, kernel_name));

  constexpr int kLanes = 32;
  std::vector<unsigned int> h_in(kLanes);
  for (int i = 0; i < kLanes; ++i) h_in[i] = 1000u + (unsigned int)i * 3u;

  CUdeviceptr d_buf;
  CUCHK(cuMemAlloc(&d_buf, kLanes * sizeof(unsigned int)));
  CUCHK(cuMemcpyHtoD(d_buf, h_in.data(), kLanes * sizeof(unsigned int)));

  // Kernel param packing for Driver API: each entry points to the argument value.
  unsigned long long base = (unsigned long long)d_buf;
  void *args[] = {&base};

  CUCHK(cuLaunchKernel(
      fun,
      /*gridX=*/1, /*gridY=*/1, /*gridZ=*/1,
      /*blockX=*/kLanes, /*blockY=*/1, /*blockZ=*/1,
      /*sharedMemBytes=*/0,
      /*hStream=*/0,
      args,
      /*extra=*/nullptr));

  CUCHK(cuCtxSynchronize());

  std::vector<unsigned int> h_out(kLanes);
  CUCHK(cuMemcpyDtoH(h_out.data(), d_buf, kLanes * sizeof(unsigned int)));

  int bad = 0;
  for (int i = 0; i < kLanes; ++i) {
    unsigned int expect = h_in[i] + (unsigned int)i;
    if (h_out[i] != expect) {
      if (bad < 8) {
        std::fprintf(stderr, "mismatch lane %d: got=%u expect=%u\n", i, h_out[i], expect);
      }
      bad++;
    }
  }

  CUCHK(cuMemFree(d_buf));
  CUCHK(cuModuleUnload(mod));
  CUCHK(cuDevicePrimaryCtxRelease(dev));

  if (bad) {
    std::fprintf(stderr, "FAIL: %d mismatches\n", bad);
    return 2;
  }

  std::printf("OK: vecadd_simple matched for %d lanes\n", kLanes);
  return 0;
}
