#include <cuda.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

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

static void write_u32_le(std::vector<std::uint8_t> &heap, std::size_t off,
                         std::uint32_t v) {
  heap[off + 0] = (std::uint8_t)(v & 0xffu);
  heap[off + 1] = (std::uint8_t)((v >> 8) & 0xffu);
  heap[off + 2] = (std::uint8_t)((v >> 16) & 0xffu);
  heap[off + 3] = (std::uint8_t)((v >> 24) & 0xffu);
}

static void write_f32_le(std::vector<std::uint8_t> &heap, std::size_t off,
                         float f) {
  std::uint32_t bits;
  static_assert(sizeof(bits) == sizeof(f));
  std::memcpy(&bits, &f, sizeof(bits));
  write_u32_le(heap, off, bits);
}

int main(int argc, char **argv) {
  const char *cubin_path = (argc >= 2) ? argv[1] : "build/vecadd.cubin";
  const char *kernel_name = (argc >= 3) ? argv[2] : "ventus_start";

  // Problem sizes to validate.
  const std::vector<std::uint32_t> test_sizes = {32u, 256u, 1000u};

  CUCHK(cuInit(0));

  CUdevice dev;
  CUCHK(cuDeviceGet(&dev, 0));

  CUcontext ctx;
  CUCHK(cuDevicePrimaryCtxRetain(&ctx, dev));
  CUCHK(cuCtxSetCurrent(ctx));

  auto cubin = read_file(cubin_path);

  CUmodule mod;
  CUCHK(cuModuleLoadData(&mod, cubin.data()));

  CUfunction fun;
  CUCHK(cuModuleGetFunction(&fun, mod, kernel_name));

  // Ventus ABI emulation constants.
  constexpr std::uint32_t VENTUS_BASE = 0x90000000u;

  // Offsets inside metadata buffer (u32 fields), per lab/01_host_device_abi/reference/ventus.h
  constexpr std::size_t KNL_ENTRY = 0;
  constexpr std::size_t KNL_ARG_BASE = 4;
  constexpr std::size_t KNL_WORK_DIM = 8;
  constexpr std::size_t KNL_GL_SIZE_X = 12;
  constexpr std::size_t KNL_GL_SIZE_Y = 16;
  constexpr std::size_t KNL_GL_SIZE_Z = 20;
  constexpr std::size_t KNL_LC_SIZE_X = 24;
  constexpr std::size_t KNL_LC_SIZE_Y = 28;
  constexpr std::size_t KNL_LC_SIZE_Z = 32;
  constexpr std::size_t KNL_GL_OFFSET_X = 36;
  constexpr std::size_t KNL_GL_OFFSET_Y = 40;
  constexpr std::size_t KNL_GL_OFFSET_Z = 44;
  constexpr std::size_t KNL_PRINT_ADDR = 48;
  constexpr std::size_t KNL_PRINT_SIZE = 52;

  // Heap layout (all addresses are Ventus virtual addresses = VENTUS_BASE + offset).
  // Keep simple and aligned.
  constexpr std::size_t HEAP_SIZE = 1u << 20; // 1 MiB
  constexpr std::size_t OFF_METADATA = 0x0000;
  constexpr std::size_t OFF_ARGBUF = 0x0100;
  constexpr std::size_t OFF_A = 0x1000;

  for (std::uint32_t N : test_sizes) {
    // Choose a block size and launch enough blocks to cover N.
    const std::uint32_t block_x = 256u;
    const std::uint32_t grid_x = (N + block_x - 1u) / block_x;

    // Pack heap contents on host.
    std::vector<std::uint8_t> heap(HEAP_SIZE, 0);

    const std::size_t bytes = (std::size_t)N * sizeof(float);
    const std::size_t OFF_B = OFF_A + bytes;
    const std::size_t OFF_C = OFF_B + bytes;

    if (OFF_C + bytes > HEAP_SIZE) {
      std::fprintf(stderr, "heap too small for N=%u\n", N);
      return 2;
    }

    // Ventus virtual addresses (u32) for each region.
    const std::uint32_t knl_addr = VENTUS_BASE + (std::uint32_t)OFF_METADATA;
    const std::uint32_t arg_base = VENTUS_BASE + (std::uint32_t)OFF_ARGBUF;
    const std::uint32_t a_addr = VENTUS_BASE + (std::uint32_t)OFF_A;
    const std::uint32_t b_addr = VENTUS_BASE + (std::uint32_t)OFF_B;
    const std::uint32_t c_addr = VENTUS_BASE + (std::uint32_t)OFF_C;

    // Fill metadata buffer.
    write_u32_le(heap, OFF_METADATA + KNL_ENTRY, 0u); // unused by PTX
    write_u32_le(heap, OFF_METADATA + KNL_ARG_BASE, arg_base);
    write_u32_le(heap, OFF_METADATA + KNL_WORK_DIM, 1u);
    write_u32_le(heap, OFF_METADATA + KNL_GL_SIZE_X, N);
    write_u32_le(heap, OFF_METADATA + KNL_GL_SIZE_Y, 1u);
    write_u32_le(heap, OFF_METADATA + KNL_GL_SIZE_Z, 1u);
    write_u32_le(heap, OFF_METADATA + KNL_LC_SIZE_X, block_x); // match blockDim.x
    write_u32_le(heap, OFF_METADATA + KNL_LC_SIZE_Y, 1u);
    write_u32_le(heap, OFF_METADATA + KNL_LC_SIZE_Z, 1u);
    write_u32_le(heap, OFF_METADATA + KNL_GL_OFFSET_X, 0u);
    write_u32_le(heap, OFF_METADATA + KNL_GL_OFFSET_Y, 0u);
    write_u32_le(heap, OFF_METADATA + KNL_GL_OFFSET_Z, 0u);
    write_u32_le(heap, OFF_METADATA + KNL_PRINT_ADDR, 0u);
    write_u32_le(heap, OFF_METADATA + KNL_PRINT_SIZE, 0u);

    // Fill arg buffer: 3 x u32 pointers.
    write_u32_le(heap, OFF_ARGBUF + 0, a_addr);
    write_u32_le(heap, OFF_ARGBUF + 4, b_addr);
    write_u32_le(heap, OFF_ARGBUF + 8, c_addr);

    // Fill inputs and expected.
    std::vector<float> expect(N);
    for (std::uint32_t i = 0; i < N; ++i) {
      const float a = (float)i * 0.25f;
      const float b = (float)i * -0.5f + 3.0f;
      write_f32_le(heap, OFF_A + (std::size_t)i * 4, a);
      write_f32_le(heap, OFF_B + (std::size_t)i * 4, b);
      write_f32_le(heap, OFF_C + (std::size_t)i * 4, 0.0f);
      expect[i] = a + b;
    }

    CUdeviceptr d_heap;
    CUCHK(cuMemAlloc(&d_heap, HEAP_SIZE));
    CUCHK(cuMemcpyHtoD(d_heap, heap.data(), HEAP_SIZE));

    unsigned long long heap_base = (unsigned long long)d_heap;
    std::uint32_t knl_addr_param = knl_addr;
    void *args[] = {&heap_base, &knl_addr_param};

    CUCHK(cuLaunchKernel(fun, grid_x, 1, 1, block_x, 1, 1, 0, 0, args, nullptr));
    CUCHK(cuCtxSynchronize());

    // Read back just C for verification.
    std::vector<std::uint8_t> out_c(bytes);
    CUCHK(cuMemcpyDtoH(out_c.data(), d_heap + OFF_C, bytes));

    int bad = 0;
    for (std::uint32_t i = 0; i < N; ++i) {
      std::uint32_t bits = (std::uint32_t)out_c[i * 4 + 0] |
                          ((std::uint32_t)out_c[i * 4 + 1] << 8) |
                          ((std::uint32_t)out_c[i * 4 + 2] << 16) |
                          ((std::uint32_t)out_c[i * 4 + 3] << 24);
      float got;
      std::memcpy(&got, &bits, sizeof(got));
      float ex = expect[i];
      float diff = std::fabs(got - ex);
      if (!(diff <= 1e-6f)) {
        if (bad < 8) {
          std::fprintf(stderr, "mismatch N=%u i=%u got=%f expect=%f\n", N, i, got,
                       ex);
        }
        bad++;
      }
    }

    CUCHK(cuMemFree(d_heap));

    if (bad) {
      std::fprintf(stderr, "FAIL: N=%u mismatches=%d\n", N, bad);
      CUCHK(cuModuleUnload(mod));
      CUCHK(cuDevicePrimaryCtxRelease(dev));
      return 2;
    }

    std::printf("OK: ventus_start vecadd matched for N=%u (grid=%u block=%u)\n", N,
                grid_x, block_x);
  }

  CUCHK(cuModuleUnload(mod));
  CUCHK(cuDevicePrimaryCtxRelease(dev));
  return 0;
}
