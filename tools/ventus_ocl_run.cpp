#include <CL/cl.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

static const char *cl_err_str(cl_int e) {
  switch (e) {
  case CL_SUCCESS: return "CL_SUCCESS";
  case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
  case CL_DEVICE_NOT_AVAILABLE: return "CL_DEVICE_NOT_AVAILABLE";
  case CL_COMPILER_NOT_AVAILABLE: return "CL_COMPILER_NOT_AVAILABLE";
  case CL_MEM_OBJECT_ALLOCATION_FAILURE: return "CL_MEM_OBJECT_ALLOCATION_FAILURE";
  case CL_OUT_OF_RESOURCES: return "CL_OUT_OF_RESOURCES";
  case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
  case CL_PROFILING_INFO_NOT_AVAILABLE: return "CL_PROFILING_INFO_NOT_AVAILABLE";
  case CL_MEM_COPY_OVERLAP: return "CL_MEM_COPY_OVERLAP";
  case CL_IMAGE_FORMAT_MISMATCH: return "CL_IMAGE_FORMAT_MISMATCH";
  case CL_IMAGE_FORMAT_NOT_SUPPORTED: return "CL_IMAGE_FORMAT_NOT_SUPPORTED";
  case CL_BUILD_PROGRAM_FAILURE: return "CL_BUILD_PROGRAM_FAILURE";
  case CL_MAP_FAILURE: return "CL_MAP_FAILURE";
  case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
  case CL_INVALID_DEVICE_TYPE: return "CL_INVALID_DEVICE_TYPE";
  case CL_INVALID_PLATFORM: return "CL_INVALID_PLATFORM";
  case CL_INVALID_DEVICE: return "CL_INVALID_DEVICE";
  case CL_INVALID_CONTEXT: return "CL_INVALID_CONTEXT";
  case CL_INVALID_QUEUE_PROPERTIES: return "CL_INVALID_QUEUE_PROPERTIES";
  case CL_INVALID_COMMAND_QUEUE: return "CL_INVALID_COMMAND_QUEUE";
  case CL_INVALID_HOST_PTR: return "CL_INVALID_HOST_PTR";
  case CL_INVALID_MEM_OBJECT: return "CL_INVALID_MEM_OBJECT";
  case CL_INVALID_IMAGE_FORMAT_DESCRIPTOR: return "CL_INVALID_IMAGE_FORMAT_DESCRIPTOR";
  case CL_INVALID_IMAGE_SIZE: return "CL_INVALID_IMAGE_SIZE";
  case CL_INVALID_SAMPLER: return "CL_INVALID_SAMPLER";
  case CL_INVALID_BINARY: return "CL_INVALID_BINARY";
  case CL_INVALID_BUILD_OPTIONS: return "CL_INVALID_BUILD_OPTIONS";
  case CL_INVALID_PROGRAM: return "CL_INVALID_PROGRAM";
  case CL_INVALID_PROGRAM_EXECUTABLE: return "CL_INVALID_PROGRAM_EXECUTABLE";
  case CL_INVALID_KERNEL_NAME: return "CL_INVALID_KERNEL_NAME";
  case CL_INVALID_KERNEL_DEFINITION: return "CL_INVALID_KERNEL_DEFINITION";
  case CL_INVALID_KERNEL: return "CL_INVALID_KERNEL";
  case CL_INVALID_ARG_INDEX: return "CL_INVALID_ARG_INDEX";
  case CL_INVALID_ARG_VALUE: return "CL_INVALID_ARG_VALUE";
  case CL_INVALID_ARG_SIZE: return "CL_INVALID_ARG_SIZE";
  case CL_INVALID_KERNEL_ARGS: return "CL_INVALID_KERNEL_ARGS";
  case CL_INVALID_WORK_DIMENSION: return "CL_INVALID_WORK_DIMENSION";
  case CL_INVALID_WORK_GROUP_SIZE: return "CL_INVALID_WORK_GROUP_SIZE";
  case CL_INVALID_WORK_ITEM_SIZE: return "CL_INVALID_WORK_ITEM_SIZE";
  case CL_INVALID_GLOBAL_OFFSET: return "CL_INVALID_GLOBAL_OFFSET";
  case CL_INVALID_EVENT_WAIT_LIST: return "CL_INVALID_EVENT_WAIT_LIST";
  case CL_INVALID_EVENT: return "CL_INVALID_EVENT";
  case CL_INVALID_OPERATION: return "CL_INVALID_OPERATION";
  case CL_INVALID_GL_OBJECT: return "CL_INVALID_GL_OBJECT";
  case CL_INVALID_BUFFER_SIZE: return "CL_INVALID_BUFFER_SIZE";
  case CL_INVALID_MIP_LEVEL: return "CL_INVALID_MIP_LEVEL";
  case CL_INVALID_GLOBAL_WORK_SIZE: return "CL_INVALID_GLOBAL_WORK_SIZE";
  default: return "CL_UNKNOWN_ERROR";
  }
}

static void cl_check(cl_int e, std::string_view what) {
  if (e != CL_SUCCESS) {
    std::cerr << "OpenCL error: " << what << " => " << cl_err_str(e) << " (" << e << ")\n";
    std::exit(2);
  }
}

static std::string read_file(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "failed to read: " << path << "\n";
    std::exit(2);
  }
  std::string s;
  f.seekg(0, std::ios::end);
  s.resize(static_cast<size_t>(f.tellg()));
  f.seekg(0, std::ios::beg);
  f.read(s.data(), static_cast<std::streamsize>(s.size()));
  return s;
}

static uint64_t fnv1a64(const uint8_t *p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

struct Buffers final {
  std::vector<uint8_t> bytes;
  uint64_t hash = 0;
};

static void write_bytes(const std::string &path, const std::vector<uint8_t> &bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    std::cerr << "failed to write: " << path << "\n";
    std::exit(2);
  }
  f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

static std::string get_build_log(cl_program prog, cl_device_id dev) {
  size_t n = 0;
  if (clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n) != CL_SUCCESS) return "";
  std::string s(n, '\0');
  if (clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, n, s.data(), nullptr) != CL_SUCCESS) return "";
  return s;
}

static std::optional<std::vector<uint8_t>> get_program_binary_first_device(cl_program prog) {
  cl_uint num_devs = 0;
  if (clGetProgramInfo(prog, CL_PROGRAM_NUM_DEVICES, sizeof(num_devs), &num_devs, nullptr) != CL_SUCCESS) return std::nullopt;
  if (num_devs == 0) return std::nullopt;

  std::vector<size_t> sizes(num_devs);
  if (clGetProgramInfo(prog, CL_PROGRAM_BINARY_SIZES, sizeof(size_t) * num_devs, sizes.data(), nullptr) != CL_SUCCESS) return std::nullopt;
  if (sizes[0] == 0) return std::nullopt;

  std::vector<uint8_t> bin0(sizes[0]);
  std::vector<unsigned char *> bins(num_devs, nullptr);
  bins[0] = reinterpret_cast<unsigned char *>(bin0.data());
  if (clGetProgramInfo(prog, CL_PROGRAM_BINARIES, sizeof(unsigned char *) * num_devs, bins.data(), nullptr) != CL_SUCCESS) return std::nullopt;
  return bin0;
}

static Buffers run_once(cl_context ctx, cl_command_queue q, cl_device_id dev, const std::string &src, const char *kernel_name, size_t n,
                        bool force_binary, std::optional<std::vector<uint8_t>> binary_override) {
  cl_int err = CL_SUCCESS;

  cl_program prog = nullptr;
  std::vector<uint8_t> owned_bin;
  if (force_binary) {
    if (!binary_override) {
      std::cerr << "binary mode requested but no binary provided\n";
      std::exit(2);
    }
    const unsigned char *b0 = binary_override->data();
    const size_t sz0 = binary_override->size();
    cl_int bin_status = CL_SUCCESS;
    prog = clCreateProgramWithBinary(ctx, 1, &dev, &sz0, &b0, &bin_status, &err);
    if (err != CL_SUCCESS) {
      std::cerr << "clCreateProgramWithBinary failed: " << cl_err_str(err) << " (" << err << ")\n";
      std::exit(3);
    }
    cl_check(bin_status, "binary_status");
    err = clBuildProgram(prog, 1, &dev, "", nullptr, nullptr);
    if (err != CL_SUCCESS) {
      std::cerr << "clBuildProgram(binary) failed: " << cl_err_str(err) << " (" << err << ")\n";
      std::cerr << get_build_log(prog, dev) << "\n";
      std::exit(3);
    }
  } else {
    const char *p = src.c_str();
    const size_t sz = src.size();
    prog = clCreateProgramWithSource(ctx, 1, &p, &sz, &err);
    cl_check(err, "clCreateProgramWithSource");
    err = clBuildProgram(prog, 1, &dev, "", nullptr, nullptr);
    if (err != CL_SUCCESS) {
      std::cerr << "clBuildProgram(source) failed: " << cl_err_str(err) << " (" << err << ")\n";
      std::cerr << get_build_log(prog, dev) << "\n";
      std::exit(2);
    }
  }

  cl_kernel k = clCreateKernel(prog, kernel_name, &err);
  cl_check(err, "clCreateKernel");

  auto make_buf = [&](cl_mem_flags flags, const void *host, size_t bytes) -> cl_mem {
    cl_int e = CL_SUCCESS;
    cl_mem m = clCreateBuffer(ctx, flags, bytes, const_cast<void *>(host), &e);
    cl_check(e, "clCreateBuffer");
    return m;
  };

  auto enqueue_read = [&](cl_mem m, void *dst, size_t bytes) {
    cl_int e = clEnqueueReadBuffer(q, m, CL_TRUE, 0, bytes, dst, 0, nullptr, nullptr);
    cl_check(e, "clEnqueueReadBuffer");
  };

  // Micro-test convention (scheme 1): every kernel uses an A/B buffer signature:
  //   __kernel void K(__global const uint *A, __global uint *B)
  // where A and B each contain N uint32 elements.
  std::vector<uint32_t> a(n);
  for (size_t i = 0; i < n; ++i) a[i] = uint32_t(i * 2654435761u) ^ 0xdeadbeefu;

  std::vector<uint8_t> out_host(n * sizeof(uint32_t));
  cl_mem in_m = make_buf(CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, a.data(), a.size() * sizeof(uint32_t));
  cl_mem out_m = make_buf(CL_MEM_READ_WRITE, nullptr, out_host.size());

  cl_check(clSetKernelArg(k, 0, sizeof(cl_mem), &in_m), "clSetKernelArg(A)");
  cl_check(clSetKernelArg(k, 1, sizeof(cl_mem), &out_m), "clSetKernelArg(B)");

  const size_t global = n;
  const size_t local = (n >= 32 ? 32 : 1);
  cl_check(clEnqueueNDRangeKernel(q, k, 1, nullptr, &global, &local, 0, nullptr, nullptr), "clEnqueueNDRangeKernel");
  cl_check(clFinish(q), "clFinish");

  enqueue_read(out_m, out_host.data(), out_host.size());

  Buffers res;
  res.bytes = out_host;
  res.hash = fnv1a64(res.bytes.data(), res.bytes.size());

  clReleaseMemObject(in_m);
  clReleaseMemObject(out_m);
  clReleaseKernel(k);
  clReleaseProgram(prog);
  (void)owned_bin;

  return res;
}

static void usage() {
  std::cerr << "Usage:\n";
  std::cerr << "  ventus_ocl_run --src <kernels.cl> --kernel <name> [--n <elements>] [--out <file>] [--binary-roundtrip]\n";
}

} // namespace

int main(int argc, char **argv) {
  std::string src_path = "testcases/ocl_compare/kernels.cl";
  std::string kernel = "test_u32_basic";
  size_t n = 256;
  std::optional<std::string> out_path;
  bool binary_roundtrip = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--src" && i + 1 < argc) {
      src_path = argv[++i];
    } else if (a == "--kernel" && i + 1 < argc) {
      kernel = argv[++i];
    } else if (a == "--n" && i + 1 < argc) {
      n = static_cast<size_t>(std::stoull(argv[++i]));
    } else if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--binary-roundtrip") {
      binary_roundtrip = true;
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      usage();
      return 2;
    }
  }

  const std::string src = read_file(src_path);

  cl_uint nplat = 0;
  cl_check(clGetPlatformIDs(0, nullptr, &nplat), "clGetPlatformIDs(n)");
  if (nplat == 0) {
    std::cerr << "No OpenCL platform\n";
    return 2;
  }
  std::vector<cl_platform_id> plats(nplat);
  cl_check(clGetPlatformIDs(nplat, plats.data(), nullptr), "clGetPlatformIDs(list)");
  const cl_platform_id plat = plats[0];

  cl_uint ndev = 0;
  cl_int err = clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 0, nullptr, &ndev);
  cl_check(err, "clGetDeviceIDs(n)");
  if (ndev == 0) {
    std::cerr << "No OpenCL device\n";
    return 2;
  }
  std::vector<cl_device_id> devs(ndev);
  cl_check(clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, ndev, devs.data(), nullptr), "clGetDeviceIDs(list)");
  const cl_device_id dev = devs[0];

  cl_context ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
  cl_check(err, "clCreateContext");
  cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
  cl_check(err, "clCreateCommandQueue");

  const Buffers res_src = run_once(ctx, q, dev, src, kernel.c_str(), n, /*force_binary=*/false, std::nullopt);

  std::optional<std::vector<uint8_t>> bin0;
  if (binary_roundtrip) {
    // Build once more from source to get program binary.
    cl_int e = CL_SUCCESS;
    const char *p = src.c_str();
    const size_t sz = src.size();
    cl_program prog = clCreateProgramWithSource(ctx, 1, &p, &sz, &e);
    cl_check(e, "clCreateProgramWithSource(for-binary)");
    e = clBuildProgram(prog, 1, &dev, "", nullptr, nullptr);
    if (e != CL_SUCCESS) {
      std::cerr << "clBuildProgram(for-binary) failed: " << cl_err_str(e) << " (" << e << ")\n";
      std::cerr << get_build_log(prog, dev) << "\n";
      return 3;
    }
    bin0 = get_program_binary_first_device(prog);
    clReleaseProgram(prog);
    if (!bin0) {
      std::cerr << "failed to fetch program binary via clGetProgramInfo\n";
      return 3;
    }

    const Buffers res_bin = run_once(ctx, q, dev, src, kernel.c_str(), n, /*force_binary=*/true, bin0);
    if (res_bin.bytes != res_src.bytes) {
      std::cerr << "binary roundtrip output mismatch\n";
      return 3;
    }
  }

  if (out_path) write_bytes(*out_path, res_src.bytes);

  std::cout << std::dec;
  std::cout << "bytes=" << res_src.bytes.size() << " hash=0x" << std::hex << res_src.hash << std::dec;
  if (binary_roundtrip) std::cout << " binary_roundtrip=1";
  std::cout << "\n";

  clReleaseCommandQueue(q);
  clReleaseContext(ctx);
  return 0;
}
