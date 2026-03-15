#include <cuda.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#define CUCHK(expr)                                                                  \
  do {                                                                               \
    CUresult _res = (expr);                                                          \
    if (_res != CUDA_SUCCESS) {                                                      \
      const char *name = nullptr;                                                    \
      const char *desc = nullptr;                                                    \
      cuGetErrorName(_res, &name);                                                   \
      cuGetErrorString(_res, &desc);                                                 \
      std::fprintf(stderr, "CUDA driver error %d (%s): %s\n", (int)_res,             \
                   name ? name : "?", desc ? desc : "?");                            \
      std::exit(1);                                                                  \
    }                                                                                \
  } while (0)

namespace {

constexpr int kHotWords = 4;
constexpr int kPct25 = 25;
constexpr int kPct50 = 50;
constexpr int kPct75 = 75;
constexpr const char *kDefaultModuleDir = "build/generated_ptx";
constexpr std::uint32_t kDefaultSeed = 7;
constexpr int kDefaultThreads = 128;

struct Options {
  std::string module_dir = kDefaultModuleDir;
  std::string module_kind = "ptx";
  int state_words = 32;
  std::string use_mode = "pct25";
  int helper_ops = 4;
  int threads = kDefaultThreads;
  std::uint32_t seed = kDefaultSeed;
  std::vector<std::string> abis = {"vctx", "value_params", "value_blob"};
};

struct KernelRun {
  std::string abi;
  std::vector<std::uint32_t> output;
};

[[noreturn]] void die(const char *message) {
  std::fprintf(stderr, "%s\n", message);
  std::exit(2);
}

[[noreturn]] void dief(const char *fmt, const char *arg) {
  std::fprintf(stderr, fmt, arg);
  std::fprintf(stderr, "\n");
  std::exit(2);
}

std::vector<char> read_file(const std::string &path) {
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    std::perror(path.c_str());
    std::exit(2);
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size <= 0) {
    std::fprintf(stderr, "empty file: %s\n", path.c_str());
    std::exit(2);
  }
  std::vector<char> buffer((size_t)size);
  if (std::fread(buffer.data(), 1, (size_t)size, file) != (size_t)size) {
    std::fprintf(stderr, "failed to read: %s\n", path.c_str());
    std::exit(2);
  }
  std::fclose(file);
  return buffer;
}

bool is_valid_abi(std::string_view abi) {
  return abi == "vctx" || abi == "value_params" || abi == "value_blob";
}

bool is_valid_mode(std::string_view mode) {
  return mode == "pct25" || mode == "pct50" || mode == "pct75" || mode == "full" || mode == "hot4" ||
         mode == "hot4_dead";
}

bool is_valid_module_kind(std::string_view module_kind) {
  return module_kind == "ptx" || module_kind == "cubin";
}

int parse_int(const char *text, const char *flag) {
  char *end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value <= 0 || value > std::numeric_limits<int>::max()) {
    std::fprintf(stderr, "invalid integer for %s: %s\n", flag, text);
    std::exit(2);
  }
  return (int)value;
}

std::uint32_t parse_u32(const char *text, const char *flag) {
  char *end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 0);
  if (end == text || *end != '\0' || value > std::numeric_limits<std::uint32_t>::max()) {
    std::fprintf(stderr, "invalid u32 for %s: %s\n", flag, text);
    std::exit(2);
  }
  return (std::uint32_t)value;
}

void print_usage(const char *argv0) {
  std::fprintf(
      stderr,
      "Usage: %s [--module-dir DIR] [--module-kind ptx|cubin] --state-words N\n"
      "          --mode pct25|pct50|pct75|full|hot4|hot4_dead --helper-ops N [--threads N] [--seed U32]\n"
      "          [--abi ABI ...]\n",
      argv0);
}

Options parse_args(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg = argv[index];
    if (arg == "--module-dir") {
      if (++index >= argc) die("missing value for --module-dir");
      options.module_dir = argv[index];
      continue;
    }
    if (arg == "--module-kind") {
      if (++index >= argc) die("missing value for --module-kind");
      options.module_kind = argv[index];
      continue;
    }
    if (arg == "--cubin-dir") {
      if (++index >= argc) die("missing value for --cubin-dir");
      options.module_dir = argv[index];
      options.module_kind = "cubin";
      continue;
    }
    if (arg == "--state-words") {
      if (++index >= argc) die("missing value for --state-words");
      options.state_words = parse_int(argv[index], "--state-words");
      continue;
    }
    if (arg == "--mode") {
      if (++index >= argc) die("missing value for --mode");
      options.use_mode = argv[index];
      continue;
    }
    if (arg == "--helper-ops") {
      if (++index >= argc) die("missing value for --helper-ops");
      options.helper_ops = parse_int(argv[index], "--helper-ops");
      continue;
    }
    if (arg == "--threads") {
      if (++index >= argc) die("missing value for --threads");
      options.threads = parse_int(argv[index], "--threads");
      continue;
    }
    if (arg == "--seed") {
      if (++index >= argc) die("missing value for --seed");
      options.seed = parse_u32(argv[index], "--seed");
      continue;
    }
    if (arg == "--abi") {
      options.abis.clear();
      while (index + 1 < argc && argv[index + 1][0] != '-') {
        ++index;
        options.abis.push_back(argv[index]);
      }
      if (options.abis.empty()) die("missing values for --abi");
      continue;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
    print_usage(argv[0]);
    std::exit(2);
  }
  if (!is_valid_mode(options.use_mode)) die("invalid --mode");
  if (!is_valid_module_kind(options.module_kind)) die("invalid --module-kind");
  for (const std::string &abi : options.abis) {
    if (!is_valid_abi(abi)) dief("invalid ABI: %s", abi.c_str());
  }
  if (options.threads > 1024) die("--threads must be <= 1024 because the kernel uses a single block");
  return options;
}

std::string kernel_name_for_abi(std::string_view abi) {
  if (abi == "vctx") return "kernel_vctx";
  if (abi == "value_params") return "kernel_value_params";
  return "kernel_value_blob";
}

std::string case_name_for_abi(const Options &options, std::string_view abi) {
  return std::string(abi) + "_s" + std::to_string(options.state_words) + "_" + options.use_mode + "_o" +
         std::to_string(options.helper_ops);
}

std::string module_extension(const Options &options) {
  return options.module_kind == "ptx" ? ".ptx" : ".cubin";
}

std::uint32_t helper_op(std::uint32_t value, int state_index, int op_index) {
  const std::uint32_t add_imm = (std::uint32_t)(3 + ((state_index + op_index) % 29));
  const std::uint32_t xor_imm =
      (std::uint32_t)(((std::uint32_t)(state_index + 1) * 0x045D9F3Bu) ^ ((std::uint32_t)op_index * 0x9E3779B1u));
  const std::uint32_t mad_add = (std::uint32_t)(7 + ((state_index * 3 + op_index) % 19));
  const int mod = op_index % 3;
  if (mod == 0) return value + add_imm;
  if (mod == 1) return value ^ xor_imm;
  return value * 3u + mad_add;
}

int percent_used_words(int state_words, int percent) {
  const int numerator = state_words * percent + 99;
  return numerator / 100;
}

int used_words(const Options &options) {
  if (options.use_mode == "pct25") return percent_used_words(options.state_words, kPct25);
  if (options.use_mode == "pct50") return percent_used_words(options.state_words, kPct50);
  if (options.use_mode == "pct75") return percent_used_words(options.state_words, kPct75);
  if (options.use_mode == "full") return options.state_words;
  return options.state_words < kHotWords ? options.state_words : kHotWords;
}

int checksum_words(const Options &options) {
  return options.use_mode == "hot4_dead" ? kHotWords : options.state_words;
}

std::vector<std::uint32_t> cpu_reference(const Options &options) {
  std::vector<std::uint32_t> output((size_t)options.threads, 0);
  std::vector<std::uint32_t> state((size_t)options.state_words, 0);
  for (int tid = 0; tid < options.threads; ++tid) {
    for (int index = 0; index < options.state_words; ++index) {
      state[(size_t)index] = (std::uint32_t)tid + (std::uint32_t)(17 * (index + 1)) + options.seed;
    }
    for (int index = 0; index < used_words(options); ++index) {
      for (int op_index = 0; op_index < options.helper_ops; ++op_index) {
        state[(size_t)index] = helper_op(state[(size_t)index], index, op_index);
      }
    }
    std::uint32_t checksum = 0;
    for (int index = 0; index < checksum_words(options); ++index) {
      checksum += state[(size_t)index];
    }
    output[(size_t)tid] = checksum;
  }
  return output;
}

KernelRun run_kernel(CUcontext context, const Options &options, const std::string &abi) {
  CUCHK(cuCtxSetCurrent(context));
  const std::string case_name = case_name_for_abi(options, abi);
  const std::string module_path = options.module_dir + "/" + case_name + module_extension(options);
  std::vector<char> module_bytes = read_file(module_path);
  module_bytes.push_back('\0');

  CUmodule module;
  char error_log[8192] = {};
  char info_log[8192] = {};
  CUjit_option jit_options[] = {
      CU_JIT_ERROR_LOG_BUFFER,
      CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES,
      CU_JIT_INFO_LOG_BUFFER,
      CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES,
  };
  void *jit_values[] = {
      error_log,
      (void *)(uintptr_t)sizeof(error_log),
      info_log,
      (void *)(uintptr_t)sizeof(info_log),
  };
  const CUresult load_result = cuModuleLoadDataEx(&module, module_bytes.data(), 4, jit_options, jit_values);
  if (load_result != CUDA_SUCCESS) {
    const char *name = nullptr;
    const char *desc = nullptr;
    cuGetErrorName(load_result, &name);
    cuGetErrorString(load_result, &desc);
    std::fprintf(stderr, "failed to load %s (%s): %s\n", module_path.c_str(), name ? name : "?",
                 desc ? desc : "?");
    if (error_log[0] != '\0') std::fprintf(stderr, "JIT error log:\n%s\n", error_log);
    if (info_log[0] != '\0') std::fprintf(stderr, "JIT info log:\n%s\n", info_log);
    std::exit(1);
  }

  CUfunction function;
  const std::string kernel_name = kernel_name_for_abi(abi);
  CUCHK(cuModuleGetFunction(&function, module, kernel_name.c_str()));

  const size_t bytes = (size_t)options.threads * sizeof(std::uint32_t);
  CUdeviceptr output_device = 0;
  CUCHK(cuMemAlloc(&output_device, bytes));
  CUCHK(cuMemsetD8(output_device, 0, bytes));

  unsigned long long out_ptr = (unsigned long long)output_device;
  std::uint32_t seed = options.seed;
  void *args[] = {&out_ptr, &seed};

  CUCHK(cuLaunchKernel(function, 1, 1, 1, (unsigned)options.threads, 1, 1, 0, 0, args, nullptr));
  CUCHK(cuCtxSynchronize());

  KernelRun result{abi, std::vector<std::uint32_t>((size_t)options.threads, 0)};
  CUCHK(cuMemcpyDtoH(result.output.data(), output_device, bytes));

  CUCHK(cuMemFree(output_device));
  CUCHK(cuModuleUnload(module));
  return result;
}

void verify_against_reference(const std::vector<std::uint32_t> &expected, const KernelRun &run) {
  int mismatches = 0;
  for (size_t index = 0; index < expected.size(); ++index) {
    if (run.output[index] == expected[index]) continue;
    if (mismatches < 8) {
      std::fprintf(stderr, "[%s] mismatch tid=%zu got=%u expect=%u\n", run.abi.c_str(), index, run.output[index],
                   expected[index]);
    }
    mismatches++;
  }
  if (mismatches != 0) {
    std::fprintf(stderr, "[%s] runtime validation failed: %d mismatches\n", run.abi.c_str(), mismatches);
    std::exit(3);
  }
}

void verify_pairwise_equal(const KernelRun &lhs, const KernelRun &rhs) {
  int mismatches = 0;
  for (size_t index = 0; index < lhs.output.size(); ++index) {
    if (lhs.output[index] == rhs.output[index]) continue;
    if (mismatches < 8) {
      std::fprintf(stderr, "[%s vs %s] mismatch tid=%zu lhs=%u rhs=%u\n", lhs.abi.c_str(), rhs.abi.c_str(), index,
                   lhs.output[index], rhs.output[index]);
    }
    mismatches++;
  }
  if (mismatches != 0) {
    std::fprintf(stderr, "[%s vs %s] cross-ABI mismatch: %d lanes\n", lhs.abi.c_str(), rhs.abi.c_str(), mismatches);
    std::exit(4);
  }
}

void print_ok_line(const Options &options, const KernelRun &run) {
  std::printf("OK: %s s%d %s o%d seed=%u lanes=%d\n", run.abi.c_str(), options.state_words, options.use_mode.c_str(),
              options.helper_ops, options.seed, options.threads);
}

}  // namespace

int main(int argc, char **argv) {
  const Options options = parse_args(argc, argv);

  CUCHK(cuInit(0));
  CUdevice device;
  CUCHK(cuDeviceGet(&device, 0));

  CUcontext context;
  CUCHK(cuDevicePrimaryCtxRetain(&context, device));
  CUCHK(cuCtxSetCurrent(context));

  const std::vector<std::uint32_t> expected = cpu_reference(options);
  std::vector<KernelRun> runs;
  runs.reserve(options.abis.size());
  for (const std::string &abi : options.abis) {
    runs.push_back(run_kernel(context, options, abi));
    verify_against_reference(expected, runs.back());
    print_ok_line(options, runs.back());
  }
  for (size_t index = 1; index < runs.size(); ++index) {
    verify_pairwise_equal(runs[0], runs[index]);
  }
  std::printf("PASS: all requested ABIs matched CPU reference and cross-ABI outputs\n");

  CUCHK(cuDevicePrimaryCtxRelease(device));
  return 0;
}
