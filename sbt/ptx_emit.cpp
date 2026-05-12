#include "sbt/ptx_emit_internal.hpp"

#include <utility>

namespace sbt::ptx {

EmitError::EmitError(std::string code_, std::string func_, uint32_t pc_, std::string detail)
    : std::runtime_error(code_ + " func=" + func_ + " pc=" + detail::hex_u32(pc_) + (detail.empty() ? "" : (" " + detail))),
      code(std::move(code_)),
      func(std::move(func_)),
      pc(pc_) {}

EmitResult emit_module(const sbt::cfg::FunctionCfg &entry_cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &entry_name, const std::vector<FuncToEmit> &funcs,
                       const std::unordered_map<uint32_t, std::string> &ptx_name_by_addr, const Options &opt) {
  std::ostringstream out;
  out << ".version 7.8\n";
  out << ".target sm_" << opt.sm << "\n";
  out << ".address_size 64\n\n";
  out << ".extern .shared .align 16 .b8 __sbt_shmem[];\n";
  out << ".shared .align 4 .u32 __sbt_pds_block_idx;\n";
  out << ".shared .align 4 .u32 __sbt_pds_wg_base;\n";
  out << ".shared .align 4 .u32 __sbt_pds_exit_count;\n\n";
  out << ".shared .align 4 .u32 __sbt_workgroup_broadcast_slot;\n\n";

  detail::ModuleInfo mod;
  mod.ptx_name_by_addr = &ptx_name_by_addr;

  for (const auto &func : funcs) {
    detail::emit_helper_func_signature(out, func.ptx_name);
    out << ";\n\n";
  }

  for (const auto &func : funcs) {
    detail::EmitCtx ctx(func.cfg, sym_by_addr, func.name, func.ptx_name, opt, mod, /*is_entry=*/false);
    ctx.emit_body();
    out << ctx.out.str();
  }

  detail::EmitCtx entry(entry_cfg, sym_by_addr, entry_name, entry_name, opt, mod, /*is_entry=*/true);
  entry.emit_body();
  out << entry.out.str();

  EmitResult result;
  result.ptx = out.str();
  return result;
}

EmitResult emit_kernel(const sbt::cfg::FunctionCfg &cfg, const std::unordered_map<uint32_t, std::string> &sym_by_addr,
                       const std::string &kernel_name, const Options &opt) {
  const std::unordered_map<uint32_t, std::string> empty;
  const std::vector<FuncToEmit> none;
  return emit_module(cfg, sym_by_addr, kernel_name, none, empty, opt);
}

} // namespace sbt::ptx
