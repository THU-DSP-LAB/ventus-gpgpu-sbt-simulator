// Minimal OpenCL kernels for Spike-vs-PTX output comparison.

__kernel void test_u32_basic(__global const uint *in, __global uint *out) {
  const uint i = (uint)get_global_id(0);
  uint a = in[i];
  uint b = (a << 3) | (a >> 5);
  uint c = (b ^ 0x12345678u) & 0x0fffffffu;
  out[i] = c + 7u;
}

__kernel void test_u32_divrem(__global const uint *a, __global const uint *b, __global uint *out2) {
  const uint i = (uint)get_global_id(0);
  const uint x = a[i];
  const uint y = b[i]; // keep y != 0 to avoid undefined behavior in OpenCL C
  const uint q = x / y;
  out2[2 * i + 0] = q;
  out2[2 * i + 1] = x - (q * y);
}

__kernel void test_i8_i16_load(__global const char *in8,
                               __global const uchar *inu8,
                               __global const short *in16,
                               __global const ushort *inu16,
                               __global int *out4) {
  const uint i = (uint)get_global_id(0);
  out4[4 * i + 0] = (int)in8[i];
  out4[4 * i + 1] = (int)inu8[i];
  out4[4 * i + 2] = (int)in16[i];
  out4[4 * i + 3] = (int)inu16[i];
}

__kernel void test_store_i8_i16(__global const uint *in,
                                __global char *out8,
                                __global short *out16,
                                __global uint *out32) {
  const uint i = (uint)get_global_id(0);
  const uint x = in[i];
  out8[i] = (char)x;
  out16[i] = (short)(x ^ 0x55aa55aau);
  out32[i] = (x << 1) + 3u;
}
