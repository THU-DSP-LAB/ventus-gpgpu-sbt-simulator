#define _GNU_SOURCE

#include <cuda.h>
#include <dlfcn.h>
#include <stdio.h>

typedef CUresult (*cuLaunchKernel_fn)(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                      unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                      unsigned int sharedMemBytes, CUstream hStream, void **kernelParams, void **extra);

CUresult cuLaunchKernel(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ, unsigned int blockDimX,
                        unsigned int blockDimY, unsigned int blockDimZ, unsigned int sharedMemBytes, CUstream hStream,
                        void **kernelParams, void **extra) {
  static cuLaunchKernel_fn real = NULL;
  if (!real) real = (cuLaunchKernel_fn)dlsym(RTLD_NEXT, "cuLaunchKernel");

  fprintf(stderr,
          "[cu_hook] cuLaunchKernel f=%p grid=%u,%u,%u block=%u,%u,%u shmem=%u stream=%p params=%p extra=%p\n",
          (void *)f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, (void *)hStream,
          (void *)kernelParams, (void *)extra);
  fflush(stderr);

  return real(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream, kernelParams, extra);
}

