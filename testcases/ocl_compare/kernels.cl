// OpenCL micro-tests for Spike-vs-PTX comparison + instruction coverage.
//
// Convention (scheme 1):
//   __kernel void K(__global const uint *A, __global uint *B)
// with A and B each containing N uint32 elements.

static inline uint u32_rotl(uint x, uint n) { return (x << n) | (x >> (32u - n)); }

__kernel void mt_int_all(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const uint z = A[gid ^ 2u] | 1u;

  uint out = x;

  // Force SIMT control-flow (setrpc/join + vector branch).
  if ((x & 1u) == 0u) {
    out = x + y;
  } else {
    out = x - y;
  }

  // Force all custom vector branch mnemonics in structured form.
  if (x != y) out ^= 0x11111111u;                 // vbne
  if ((int)x < (int)y) out ^= 0x22222222u;         // vblt
  if ((int)x >= (int)y) out ^= 0x33333333u;        // vbge
  if (x < y) out ^= 0x44444444u;                   // vbltu
  if (x >= y) out ^= 0x55555555u;                  // vbgeu

  // Force load/store of multiple element sizes (vl*b/hl* + vs*b/hs*).
  const __global char *A_s8 = (const __global char *)A;
  const __global uchar *A_u8 = (const __global uchar *)A;
  const __global short *A_s16 = (const __global short *)A;
  const __global ushort *A_u16 = (const __global ushort *)A;

  const char s8 = A_s8[4u * gid + 0u];
  const uchar u8 = A_u8[4u * gid + 1u];
  const short s16 = A_s16[2u * gid + 0u];
  const ushort u16 = A_u16[2u * gid + 1u];

  const int si = (int)s8 + (int)s16;
  const uint ui = (uint)u8 + (uint)u16;
  out ^= (uint)si;
  out ^= ui;

  __global uchar *B_u8 = (__global uchar *)B;
  __global ushort *B_u16 = (__global ushort *)B;
  B_u8[4u * gid + 0u] = (uchar)(out ^ 0xa5u);
  B_u8[4u * gid + 1u] = (uchar)(out >> 8);
  B_u16[2u * gid + 1u] = (ushort)(out ^ 0x5a5au);

  // Force barrier instruction (custom).
  barrier(CLK_GLOBAL_MEM_FENCE);

  // Keep a simple, deterministic output for exact compare.
  out = (out + u32_rotl(y, 3)) ^ (z * 17u);
  B[gid] = out;
}

__kernel void mt_vbranch_lt(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];

  uint out = x;
  if ((int)x < (int)y) out ^= 0x13579bdfu; // vblt
  if (x < y) out ^= 0x2468ace0u;           // vbltu
  B[gid] = out;
}

__kernel void mt_float_all(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const uint z = A[gid ^ 2u];

  // Avoid libcall-based math; keep everything inlined / instruction-based.
  const float fx = ((float)(x & 0xffu)) * 0.01f;
  const float fy = ((float)(y & 0xffu)) * 0.02f + 1.0f;
  const float fz = ((float)(z & 0xffu)) * 0.03f + 0.5f;

  float r = (fx * fy) + fz;
  r = r / (fy + 0.25f);
  if (r < 0.0f) r = -r;

  float s;
  __asm__ __volatile__("vfsqrt.v %0, %1\n" : "=vr"(s) : "vr"(r));
  float e;
  __asm__ __volatile__("vfexp %0, %1\n" : "=vr"(e) : "vr"(s));
  B[gid] = as_uint(e);
}

__kernel void mt_csr_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  (void)A;

  uint out = 0u;
  uint t = 0u;

  // zicsr: use CSR_WID (0x805), expected to be stable and 0 for minimal NDRange.
  uint old = 0u;
  __asm__ __volatile__("csrrs %0, 0x805, x0\n" : "=r"(old));
  out ^= old;
  __asm__ __volatile__("csrrw %0, 0x805, %1\n" : "=r"(t) : "r"(old));
  out ^= t;
  __asm__ __volatile__("csrrc %0, 0x805, x0\n" : "=r"(t));
  out ^= t;
  __asm__ __volatile__("csrrwi %0, 0x805, 0\n" : "=r"(t));
  out ^= t;
  __asm__ __volatile__("csrrsi %0, 0x805, 0\n" : "=r"(t));
  out ^= t;
  __asm__ __volatile__("csrrci %0, 0x805, 0\n" : "=r"(t));
  out ^= t;

  B[gid] = out ^ (gid * 17u);
}

__kernel void mt_mext_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint a = A[0] | 1u;
  const uint b = A[1] | 3u;
  const uint c = A[2] | 5u;

  uint out = 0u;
  uint t = 0u;

  // M-extension: use non-zero divisors.
  __asm__ __volatile__("mul %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;
  __asm__ __volatile__("mulh %0, %1, %2\n" : "=r"(t) : "r"((int)a), "r"((int)b));
  out ^= t;
  __asm__ __volatile__("mulhsu %0, %1, %2\n" : "=r"(t) : "r"((int)a), "r"(b));
  out ^= t;
  __asm__ __volatile__("mulhu %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;

  __asm__ __volatile__("div %0, %1, %2\n" : "=r"(t) : "r"((int)a), "r"((int)(c | 1u)));
  out ^= t;
  __asm__ __volatile__("divu %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(c | 1u));
  out ^= t;
  __asm__ __volatile__("rem %0, %1, %2\n" : "=r"(t) : "r"((int)b), "r"((int)(c | 1u)));
  out ^= t;
  __asm__ __volatile__("remu %0, %1, %2\n" : "=r"(t) : "r"(b), "r"(c | 1u));
  out ^= t;

  B[gid] = out ^ u32_rotl(a, 1) ^ (gid * 3u);
}

__kernel void mt_rv32i_alu_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint a = A[0] | 1u;
  const uint b = A[1] | 3u;

  uint out = 0u;
  uint t = 0u;

  __asm__ __volatile__("and %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;
  __asm__ __volatile__("andi %0, %1, 15\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("or %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;
  __asm__ __volatile__("ori %0, %1, 42\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("xor %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;
  __asm__ __volatile__("xori %0, %1, 99\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("sub %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;

  const uint shamt = b & 31u;
  __asm__ __volatile__("sll %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(shamt));
  out ^= t;
  __asm__ __volatile__("slli %0, %1, 3\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("srl %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(shamt));
  out ^= t;
  __asm__ __volatile__("srli %0, %1, 5\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("sra %0, %1, %2\n" : "=r"(t) : "r"((int)a), "r"(shamt));
  out ^= t;
  __asm__ __volatile__("srai %0, %1, 7\n" : "=r"(t) : "r"((int)a));
  out ^= t;

  __asm__ __volatile__("slt %0, %1, %2\n" : "=r"(t) : "r"((int)a), "r"((int)b));
  out ^= t;
  __asm__ __volatile__("slti %0, %1, -7\n" : "=r"(t) : "r"((int)a));
  out ^= t;
  __asm__ __volatile__("sltiu %0, %1, 13\n" : "=r"(t) : "r"(a));
  out ^= t;
  __asm__ __volatile__("sltu %0, %1, %2\n" : "=r"(t) : "r"(a), "r"(b));
  out ^= t;

  B[gid] = out ^ u32_rotl(a, 3) ^ (gid * 11u);
}

__kernel void mt_rv32i_mem_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint a = A[0] | 1u;
  const uint b = A[1] | 3u;

  uint out = 0u;
  uint t = 0u;
  int ts = 0;

  // Exercise byte/halfword load/store in a single lane to keep the generated code small and deterministic.
  if (gid == 0u) {
    __global uchar *p = (__global uchar *)B;

    t = (a ^ 0x5au) & 0xffu;
    __asm__ __volatile__("sb %1, 0(%0)\n" : : "r"(p), "r"(t) : "memory");
    __asm__ __volatile__("lb %0, 0(%1)\n" : "=r"(ts) : "r"(p) : "memory");
    out ^= (uint)ts;
    __asm__ __volatile__("lbu %0, 0(%1)\n" : "=r"(t) : "r"(p) : "memory");
    out ^= t;

    t = (b ^ 0x5a5au) & 0xffffu;
    __asm__ __volatile__("sh %1, 2(%0)\n" : : "r"(p), "r"(t) : "memory");
    __asm__ __volatile__("lh %0, 2(%1)\n" : "=r"(ts) : "r"(p) : "memory");
    out ^= (uint)ts;
    __asm__ __volatile__("lhu %0, 2(%1)\n" : "=r"(t) : "r"(p) : "memory");
    out ^= t;

    out ^= u32_rotl(a, 7) ^ (b * 5u);
  }

  B[gid] = out;
}

__kernel void mt_rv32i_branch_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint a = A[0] | 1u;
  const uint b = A[1] | 3u;

  uint br0, br1, br2;
  __asm__ __volatile__(
      "blt %1, %2, 1f\n"
      "addi %0, x0, 3\n"
      "bge x0, x0, 2f\n"
      "1:\n"
      "addi %0, x0, 7\n"
      "2:\n"
      : "=r"(br0)
      : "r"((int)a), "r"((int)b));
  __asm__ __volatile__(
      "bgeu %1, %2, 1f\n"
      "addi %0, x0, 11\n"
      "bge x0, x0, 2f\n"
      "1:\n"
      "addi %0, x0, 13\n"
      "2:\n"
      : "=r"(br1)
      : "r"(a), "r"(b));

  __asm__ __volatile__(
      "bne %1, %2, 1f\n"
      "addi %0, x0, 17\n"
      "bge x0, x0, 2f\n"
      "1:\n"
      "addi %0, x0, 19\n"
      "2:\n"
      : "=r"(br2)
      : "r"(a), "r"(b));

  B[gid] = (br0 ^ u32_rotl(br1, 9) ^ u32_rotl(br2, 3)) ^ (gid * 19u);
}

__kernel void mt_int_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const uint z = A[gid ^ 2u] | 1u;
  const uint sx = 7u;

  uint acc = x ^ 0x9e3779b9u;

  // Custom/Ventus instruction.
  __asm__ __volatile__("vadd12.vi %0, %0, 1\n" : "+vr"(acc));

  // INT V mnemonics: keep results observable by hashing into acc.
  __asm__ __volatile__("vadd.vi %0, %0, 3\n" : "+vr"(acc));
  __asm__ __volatile__("vadd.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vadd.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  __asm__ __volatile__("vand.vi %0, %0, 15\n" : "+vr"(acc));
  __asm__ __volatile__("vand.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(z));
  __asm__ __volatile__("vand.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  uint t = acc;
  __asm__ __volatile__("vdiv.vv %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "vr"(z));
  acc ^= t;
  __asm__ __volatile__("vdiv.vx %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "r"(sx | 1u));
  acc ^= t;
  __asm__ __volatile__("vdivu.vv %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "vr"(z));
  acc ^= t;
  __asm__ __volatile__("vdivu.vx %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "r"(sx | 1u));
  acc ^= t;

  // Multiply-accumulate (vx forms via .insn to avoid assembler mnemonic issues).
  uint mac = acc;
  __asm__ __volatile__("vmacc.vv %0, %1, %2\n" : "+vr"(mac) : "vr"(x), "vr"(y));
  __asm__ __volatile__(".insn r 0x57, 0x6, 0x5b, %0, %1, %2\n" : "+vr"(mac) : "r"(sx), "vr"(y));
  acc ^= mac;

  uint mad = acc;
  __asm__ __volatile__("vmadd.vv %0, %1, %2\n" : "+vr"(mad) : "vr"(x), "vr"(y));
  __asm__ __volatile__(".insn r 0x57, 0x6, 0x53, %0, %1, %2\n" : "+vr"(mad) : "r"(sx), "vr"(y));
  acc ^= mad;

  // Min/max.
  __asm__ __volatile__("vmax.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmax.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vmaxu.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmaxu.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vmin.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmin.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vminu.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vminu.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  // Comparisons -> mask -> observe one bit via vmv.x.s.
  uint m;
  uint mb;
  uint bit;

  __asm__ __volatile__("vmseq.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmseq.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmseq.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bit) : "vr"(m));
  acc ^= bit;

  __asm__ __volatile__("vmsgt.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmsgt.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmsgtu.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmsgtu.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmsle.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmsle.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmsle.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmsleu.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmsleu.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmsleu.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmslt.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmslt.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmsltu.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmsltu.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__("vmsne.vi %0, %1, 0\n" : "=vr"(m) : "vr"(x));
  __asm__ __volatile__("vmsne.vv %0, %1, %2\n" : "=vr"(m) : "vr"(x), "vr"(y));
  __asm__ __volatile__("vmsne.vx %0, %1, %2\n" : "=vr"(m) : "vr"(x), "r"(sx));
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bit) : "vr"(m));
  acc ^= u32_rotl(bit, 7);

  // Mask logical ops (mm) over the produced mask; observe via vmv.x.s.
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x33, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmand_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x31, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmandn_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x3b, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmnand_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x3d, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmnor_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x35, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmor_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x39, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmorn_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x3f, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmxnor_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x37, %0, %1, %1\n" : "=vr"(mb) : "vr"(m)); // vmxor_mm
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bit) : "vr"(mb)); // vmv_x_s
  acc ^= u32_rotl(bit, 13);

  // Shift/sub/xor/or family.
  __asm__ __volatile__("vor.vi %0, %0, 1\n" : "+vr"(acc));
  __asm__ __volatile__("vor.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vor.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vxor.vi %0, %0, 3\n" : "+vr"(acc));
  __asm__ __volatile__("vxor.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vxor.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vsub.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vsub.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  __asm__ __volatile__("vsll.vi %0, %0, 1\n" : "+vr"(acc));
  __asm__ __volatile__("vsll.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vsll.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vsra.vi %0, %0, 1\n" : "+vr"(acc));
  __asm__ __volatile__("vsra.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vsra.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vsrl.vi %0, %0, 1\n" : "+vr"(acc));
  __asm__ __volatile__("vsrl.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vsrl.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  // Remainder and reverse-sub.
  __asm__ __volatile__("vrem.vv %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "vr"(z));
  acc ^= t;
  __asm__ __volatile__("vrem.vx %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "r"(sx | 1u));
  acc ^= t;
  __asm__ __volatile__("vremu.vv %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "vr"(z));
  acc ^= t;
  __asm__ __volatile__("vremu.vx %0, %1, %2\n" : "=vr"(t) : "vr"(acc), "r"(sx | 1u));
  acc ^= t;
  __asm__ __volatile__("vrsub.vi %0, %0, 7\n" : "+vr"(acc));
  __asm__ __volatile__("vrsub.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  // Multiply family.
  __asm__ __volatile__("vmul.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmul.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vmulh.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmulh.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vmulhsu.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmulhsu.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));
  __asm__ __volatile__("vmulhu.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(y));
  __asm__ __volatile__("vmulhu.vx %0, %0, %1\n" : "+vr"(acc) : "r"(sx));

  // Negative multiply-accumulate/sub (vx via .insn).
  uint nms = acc;
  __asm__ __volatile__("vnmsac.vv %0, %1, %2\n" : "+vr"(nms) : "vr"(x), "vr"(y));
  __asm__ __volatile__(".insn r 0x57, 0x6, 0x5f, %0, %1, %2\n" : "+vr"(nms) : "r"(sx), "vr"(y));
  acc ^= nms;

  uint nms2 = acc;
  __asm__ __volatile__("vnmsub.vv %0, %1, %2\n" : "+vr"(nms2) : "vr"(x), "vr"(y));
  __asm__ __volatile__(".insn r 0x57, 0x6, 0x57, %0, %1, %2\n" : "+vr"(nms2) : "r"(sx), "vr"(y));
  acc ^= nms2;

  // vmerge/vmv family (some forms via .insn).
  uint vv_i, vv_v, vxv;
  __asm__ __volatile__(".insn r 0x57, 0x3, 0x2f, %0, %1, v0\n" : "=vr"(vv_i) : "r"(sx)); // vmv_v_i (imm=reg index)
  __asm__ __volatile__(".insn r 0x57, 0x0, 0x2f, %0, %1, v0\n" : "=vr"(vv_v) : "vr"(vv_i)); // vmv_v_v
  __asm__ __volatile__("vmv.v.x %0, %1\n" : "=vr"(vxv) : "r"(sx));
  acc ^= vv_i ^ vv_v ^ vxv;

  uint x_s;
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(x_s) : "vr"(vxv)); // vmv_x_s
  acc ^= u32_rotl(x_s, 19);

  uint s_x;
  uint v_sx;
  __asm__ __volatile__("vmv.v.x %0, %1\n" : "=vr"(v_sx) : "r"(0u));
  __asm__ __volatile__(".insn r 0x57, 0x6, 0x21, %0, %1, v0\n" : "+vr"(v_sx) : "r"(x_s)); // vmv_s_x
  acc ^= v_sx;

  uint mv0, mv1, mv2;
  __asm__ __volatile__(
      "vmseq.vv v0, %3, %4\n"
      ".insn r 0x57, 0x0, 0x2e, %0, %5, %6\n" // vmerge_vvm (always masked: vm=0)
      ".insn r 0x57, 0x4, 0x2e, %1, %7, %6\n" // vmerge_vxm (always masked: vm=0)
      ".insn r 0x57, 0x3, 0x2e, %2, %7, %6\n" // vmerge_vim (always masked: vm=0, imm = reg index)
      : "=vr"(mv0), "=vr"(mv1), "=vr"(mv2)
      : "vr"(x), "vr"(y), "vr"(vxv), "vr"(vv_v), "r"(sx)
      : "v0");
  acc ^= mv0 ^ mv1 ^ mv2;

  // regext/regexti are prefix instructions and must be followed by a real insn.
  __asm__ __volatile__(
      "regext x0, x0, 0\n"
      "vadd.vi %0, %0, 1\n"
      : "+vr"(acc));
  __asm__ __volatile__(
      "regexti x0, x0, 0\n"
      "vadd.vi %0, %0, 1\n"
      : "+vr"(acc));

  B[gid] = acc;
}

__kernel void mt_float_basic_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const uint z = A[gid ^ 2u];

  const float fx = ((float)(x & 0xffu)) * 0.01f + 0.25f;
  const float fy = ((float)(y & 0xffu)) * 0.02f + 1.0f;
  const float fz = ((float)(z & 0xffu)) * 0.03f + 0.5f;
  const uint sf = 0x3f800000u; // 1.0f (uniform scalar for *.vf forms)

  float acc = fx;

  __asm__ __volatile__("vfadd.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(fz));
  __asm__ __volatile__("vfsub.vf %0, %0, %1\n" : "+vr"(acc) : "r"(sf));
  __asm__ __volatile__("vfmul.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(fy));
  __asm__ __volatile__("vfdiv.vf %0, %0, %1\n" : "+vr"(acc) : "r"(sf));
  __asm__ __volatile__("vfrdiv.vf %0, %0, %1\n" : "+vr"(acc) : "r"(sf));
  __asm__ __volatile__("vfrsub.vf %0, %0, %1\n" : "+vr"(acc) : "r"(sf));
  __asm__ __volatile__("vfmax.vv %0, %0, %1\n" : "+vr"(acc) : "vr"(fy));
  __asm__ __volatile__("vfmin.vf %0, %0, %1\n" : "+vr"(acc) : "r"(sf));
  __asm__ __volatile__("vfsqrt.v %0, %0\n" : "+vr"(acc));

  B[gid] = as_uint(acc);
}

__kernel void mt_float_fma_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const uint z = A[gid ^ 2u];

  const float fx = ((float)(x & 0xffu)) * 0.01f + 0.25f;
  const float fy = ((float)(y & 0xffu)) * 0.02f + 1.0f;
  const float fz = ((float)(z & 0xffu)) * 0.03f + 0.5f;
  const uint sf = 0x3f000000u; // 0.5f (uniform scalar for *.vf forms)

  float acc = fx;

  // FMA family via .insn (vf forms use scalar float in X reg due to zfinx).
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x59, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfmacc_vf
  __asm__ __volatile__("vfmacc.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x51, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfmadd_vf
  __asm__ __volatile__("vfmadd.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x5d, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfmsac_vf
  __asm__ __volatile__("vfmsac.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x55, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfmsub_vf
  __asm__ __volatile__("vfmsub.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));

  __asm__ __volatile__(".insn r 0x57, 0x5, 0x5b, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfnmacc_vf
  __asm__ __volatile__("vfnmacc.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x53, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfnmadd_vf
  __asm__ __volatile__("vfnmadd.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x5f, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfnmsac_vf
  __asm__ __volatile__("vfnmsac.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x57, %0, %1, %2\n" : "+vr"(acc) : "r"(sf), "vr"(fz)); // vfnmsub_vf
  __asm__ __volatile__("vfnmsub.vv %0, %1, %2\n" : "+vr"(acc) : "vr"(fy), "vr"(fz));

  B[gid] = as_uint(acc);
}

__kernel void mt_vfmacc_vf_smoke(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  (void)A;

  const uint one = 0x3f800000u;  // 1.0f
  const uint two = 0x40000000u;  // 2.0f
  const uint half_bits = 0x3f000000u; // 0.5f

  float acc;
  float vs2;
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(acc) : "r"(one));
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vs2) : "r"(two));
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x59, %0, %1, %2\n" : "+vr"(acc) : "r"(half_bits), "vr"(vs2)); // vfmacc_vf

  B[gid] = as_uint(acc);
}

__kernel void mt_vfmv_v_f_smoke(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  (void)A;
  const uint one = 0x3f800000u; // 1.0f
  float acc;
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(acc) : "r"(one));
  B[gid] = as_uint(acc);
}

__kernel void mt_vfmacc_vv_smoke(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  (void)A;
  const uint one = 0x3f800000u;  // 1.0f
  const uint two = 0x40000000u;  // 2.0f
  const uint four = 0x40800000u; // 4.0f
  float vd;
  float vs1;
  float vs2;
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vd) : "r"(one));
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vs1) : "r"(two));
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vs2) : "r"(four));
  __asm__ __volatile__("vfmacc.vv %0, %1, %2\n" : "+vr"(vd) : "vr"(vs1), "vr"(vs2));
  B[gid] = as_uint(vd);
}

__kernel void mt_float_conv_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  const uint x = A[gid];
  const uint y = A[gid ^ 1u];
  const float fx = ((float)(x & 0xffu)) * 0.01f + 0.25f;

  float acc = fx;
  float t;

  uint cls;
  __asm__ __volatile__("vfclass.v %0, %1\n" : "=vr"(cls) : "vr"(acc));
  acc += (float)(cls & 7u);

  __asm__ __volatile__("vfcvt.f.x.v %0, %1\n" : "=vr"(t) : "vr"(x));
  acc += t * 0.001f;
  __asm__ __volatile__("vfcvt.f.xu.v %0, %1\n" : "=vr"(t) : "vr"(y));
  acc += t * 0.001f;

  uint ix, ixu;
  __asm__ __volatile__("vfcvt.x.f.v %0, %1\n" : "=vr"(ix) : "vr"(acc));
  __asm__ __volatile__("vfcvt.xu.f.v %0, %1\n" : "=vr"(ixu) : "vr"(acc));
  acc += (float)((ix ^ ixu) & 0xffu) * 0.0001f;

  uint irtz, irtzu;
  __asm__ __volatile__("vfcvt.rtz.x.f.v %0, %1\n" : "=vr"(irtz) : "vr"(acc));
  __asm__ __volatile__("vfcvt.rtz.xu.f.v %0, %1\n" : "=vr"(irtzu) : "vr"(acc));
  acc += (float)((irtz ^ irtzu) & 0xffu) * 0.0001f;

  B[gid] = as_uint(acc);
}

__kernel void mt_float_cmp_misc_cov(__global const uint *A, __global uint *B) {
  const uint gid = (uint)get_global_id(0);
  (void)A;

  // Cover remaining float sign-inject + compare mnemonics and make them observable via a scalar checksum.
  // Keep all operands as exact constants to avoid backend-dependent numeric drift.
  const uint one = 0x3f800000u; // 1.0f
  const uint half_bits = 0x3f000000u; // 0.5f
  const uint pos = 0x3f400000u; // 0.75f
  const uint neg = 0xbf400000u; // -0.75f
  const uint two = 0x40000000u; // 2.0f
  const uint neg_half = 0xbf000000u; // -0.5f
  const uint merge_cmp = 0x3f200000u; // 0.625f

  float vpos;
  float vneg;
  const uint pos_bits = pos;
  const uint neg_bits = neg;
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vpos) : "r"(pos_bits));
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(vneg) : "r"(neg_bits));

  uint v_zero;
  __asm__ __volatile__("vmv.v.x %0, %1\n" : "=vr"(v_zero) : "r"(0u));

  const uint zero_f = 0x00000000u;         // 0.0f
  const uint three_halves = 0x3fc00000u;   // 1.5f
  const uint cmp_08125 = 0x3f500000u;      // 0.8125f

  uint out = 0x6c8e9cf5u ^ gid;
  uint lane0;
  uint bits;

  // Sign-inject family (vv + vf forms).
  float r;
  __asm__ __volatile__("vfsgnj.vv %0, %1, %2\n" : "=vr"(r) : "vr"(vpos), "vr"(vneg)); // vfsgnj_vv
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bits) : "vr"(r)); // vmv_x_s
  out ^= bits;

  __asm__ __volatile__("vfsgnjx.vv %0, %1, %2\n" : "=vr"(r) : "vr"(vpos), "vr"(vneg)); // vfsgnjx_vv
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bits) : "vr"(r)); // vmv_x_s
  out ^= u32_rotl(bits, 7);

  __asm__ __volatile__("vfsgnjn.vf %0, %1, %2\n" : "=vr"(r) : "vr"(vpos), "r"(neg)); // vfsgnjn_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bits) : "vr"(r)); // vmv_x_s
  out ^= u32_rotl(bits, 13);

  // Float ALU family (missing vv/vf forms): observe via compare masks (v0 -> lane0).
  __asm__ __volatile__("vfmv.v.f %0, %1\n" : "=vr"(r) : "r"(pos_bits)); // 0.5/0.75

  __asm__ __volatile__("vfadd.vf %0, %0, %1\n" : "+vr"(r) : "r"(one)); // vfadd_vf
  __asm__ __volatile__("vmfeq.vf v0, %0, %1\n" : : "vr"(r), "r"(three_halves) : "v0"); // vmfeq_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 0;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vfmul.vf %0, %0, %1\n" : "+vr"(r) : "r"(half_bits)); // vfmul_vf
  __asm__ __volatile__("vmflt.vf v0, %0, %1\n" : : "vr"(r), "r"(cmp_08125) : "v0"); // vmflt_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 1;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vfsub.vv %0, %0, %1\n" : "+vr"(r) : "vr"(vneg)); // vfsub_vv
  __asm__ __volatile__("vmfle.vf v0, %0, %1\n" : : "vr"(r), "r"(three_halves) : "v0"); // vmfle_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 2;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vfmin.vv %0, %0, %1\n" : "+vr"(r) : "vr"(vpos)); // vfmin_vv
  __asm__ __volatile__("vfmax.vf %0, %0, %1\n" : "+vr"(r) : "r"(one)); // vfmax_vf

  __asm__ __volatile__("vfsgnj.vf %0, %0, %1\n" : "+vr"(r) : "r"(neg)); // vfsgnj_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bits) : "vr"(r)); // vmv_x_s
  out ^= u32_rotl(bits, 19);

  __asm__ __volatile__("vfsgnjx.vf %0, %0, %1\n" : "+vr"(r) : "r"(pos)); // vfsgnjx_vf
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, %1\n" : "=r"(bits) : "vr"(r)); // vmv_x_s
  out ^= u32_rotl(bits, 23);
  __asm__ __volatile__("vmfne.vv v0, %0, %1\n" : : "vr"(r), "vr"(vpos) : "v0"); // vmfne_vv
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 4;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  // Float compares produce a mask in v0; materialize it into 0/1 via vmerge_vxm and fold lane0 into checksum.
  __asm__ __volatile__("vmfeq.vv v0, %0, %0\n" : : "vr"(vpos) : "v0"); // vmfeq_vv (true)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 5;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vmfle.vv v0, %0, %1\n" : : "vr"(vneg), "vr"(vpos) : "v0"); // vmfle_vv (true)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 6;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vmfne.vf v0, %0, %1\n" : : "vr"(vpos), "r"(two) : "v0"); // vmfne_vf (true)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 7;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vmfgt.vf v0, %0, %1\n" : : "vr"(vpos), "r"(neg) : "v0"); // vmfgt_vf (true: rs1 < vs2)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 8;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  __asm__ __volatile__("vmfge.vf v0, %0, %1\n" : : "vr"(vpos), "r"(pos) : "v0"); // vmfge_vf (true: rs1 <= vs2)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 9;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  // Extra vmfne.vv coverage (true) + vfmerge.vfm.
  __asm__ __volatile__("vmfne.vv v0, %0, %1\n" : : "vr"(vpos), "vr"(vneg) : "v0"); // vmfne_vv (true)
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 10;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  float merged;
  __asm__ __volatile__("vmflt.vf v0, %0, %1\n" : : "vr"(vpos), "r"(merge_cmp) : "v0"); // set mask (vpos < 0.625)
  __asm__ __volatile__(".insn r 0x57, 0x5, 0x2e, %0, %2, %1\n" : "=vr"(merged) : "vr"(vpos), "r"(two) : "v0"); // vfmerge_vfm (masked)
  __asm__ __volatile__("vmfeq.vf v0, %0, %1\n" : : "vr"(merged), "r"(two) : "v0"); // observe vfmerge result
  __asm__ __volatile__(".insn r 0x57, 0x2, 0x21, %0, x0, v0\n" : "=r"(lane0)); // vmv_x_s
  out ^= (uint)(lane0 != 0u) << 14;
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0"); // restore all-ones mask

  // Restore all-ones mask before the compiler-generated store, in case it is masked by v0.
  __asm__ __volatile__("vmseq.vv v0, %0, %0\n" : : "vr"(v_zero) : "v0");
  B[gid] = out;
}
