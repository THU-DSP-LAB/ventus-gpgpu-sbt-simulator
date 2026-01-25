## Context
`testcases/simple/simple.S` is a minimal Ventus program that uses vector instructions to compute per-lane addresses and perform a load/add/store.

The Ventus semantics confirmed by the user:
- `vid.v` yields the warp lane id in [0, 31].
- `vlw12.v` / `vsw12.v` perform 32-bit loads/stores; the "12" refers to the immediate-encoding form; registers are 32-bit (no extension concerns).
- `endprg` ends the warp (no additional side effects).

## Key Decisions
### 1) Addressing model
- Do NOT attempt to force device memory to a fixed address like `0x90000000`.
- Use a kernel parameter `base` (device pointer) and treat it as the effective base corresponding to Ventus address `0x90000000`.

### 2) Thread/lane mapping
- Map Ventus lane id to PTX `%laneid`.
- For the minimal prototype, launch exactly one warp (1 block, 32 threads) to avoid multi-warp aliasing.

### 3) Memory operations
- Implement `vlw12.v`/`vsw12.v` as `ld.global.u32` and `st.global.u32` in PTX.

## Mapping Table (simple.S subset)
| Ventus | Confirmed semantics | PTX mapping (minimal) |
|---|---|---|
| `li t0, 0x90000000` | base constant | replaced by kernel param `base` (u64) |
| `vid.v v2` | lane id 0..31 | `mov.u32 r_lane, %laneid;` |
| `vsll.vi v3, v2, 2` | lane<<2 | `shl.b32 r_off, r_lane, 2;` |
| `vadd.vx v0, v3, t0` | base + off | `add.u64 rd_addr, rd_base, rd_off;` |
| `vlw12.v v1, 0(v0)` | load u32 | `ld.global.u32 r_val, [rd_addr];` |
| `vadd.vv v1, v1, v2` | add lane | `add.u32 r_val2, r_val, r_lane;` |
| `vsw12.v v1, 0(v0)` | store u32 | `st.global.u32 [rd_addr], r_val2;` |
| `endprg` | end warp | `ret;` |

## Notes on PTX parameter passing
- The `.entry` signature declares `.param` slots in order.
- Kernel reads params with `ld.param.*`.
- A pointer is passed as a 64-bit value (`.u64`), then converted with `cvta.to.global.u64` before `ld.global`/`st.global`.
