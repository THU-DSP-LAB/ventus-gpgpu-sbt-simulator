#include "sbt/ptx_emit_internal.hpp"

namespace sbt::ptx::detail {

bool try_emit_mma(EmitCtx &ctx, const sbt::DecodedInst &di) {
  if (!di.mma.valid && !(di.custom.valid && di.custom.family == sbt::CustomFamily::Mma)) return false;
  ctx.emit_mma_inst(di);
  return true;
}

} // namespace sbt::ptx::detail
