#!/usr/bin/env python3
# 背景：
# `experiment_lib.py` 持有本实验的核心生成/编译/统计逻辑。
# 当前文件只保留命令行入口，避免单文件超过仓库的可维护性上限。
#
# 需求/作用：
# 暴露稳定的执行入口 `python3 tools/run_experiment.py`，
# 同时把关键符号重新导出给测试与其它调用方使用。
#
# 用法：
#   python3 tools/run_experiment.py --out-dir build
#   python3 tools/run_experiment.py --match value_blob_input_dead_s512
#
# 实现原理/处理步骤：
# 1. 从核心库导入实验数据结构与执行函数。
# 2. 直接调用 `main()`，不在此处重复业务逻辑。
from __future__ import annotations

import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.experiment_lib import BenchCase, main, parse_ptxas_info, render_ptx

__all__ = ["BenchCase", "main", "parse_ptxas_info", "render_ptx"]


if __name__ == "__main__":
    main()
