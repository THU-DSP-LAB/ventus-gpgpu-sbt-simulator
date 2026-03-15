import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "tools" / "run_experiment.py"
SPEC = importlib.util.spec_from_file_location("run_experiment", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class RenderCaseTests(unittest.TestCase):
    def test_input_dead_blob_only_loads_hot_words(self) -> None:
        case = MODULE.BenchCase(abi="value_blob", scenario="input_dead", state_words=8, helper_ops=4)
        text = MODULE.render_ptx(case, "sm_89")
        self.assertIn(".param .align 4 .b8 in_blob[32]", text)
        self.assertIn(".func (\n    .reg .b32 out_scalar\n) helper_value_blob_input_dead(", text)
        self.assertEqual(text.count("ld.param.b32 %r16, [in_blob+0];"), 1)
        self.assertNotIn("ld.param.b32 %r8, [in_blob+16];", text)

    def test_ret_dead_blob_has_only_return_blob(self) -> None:
        case = MODULE.BenchCase(abi="value_blob", scenario="ret_dead", state_words=8, helper_ops=4)
        text = MODULE.render_ptx(case, "sm_89")
        self.assertIn("call.uni (ret_blob), helper_value_blob_ret_dead, (%r1);", text)
        self.assertNotIn("arg_blob", text)
        self.assertEqual(text.count("ld.param.b32 %r8, [ret_blob+"), case.hot_words)

    def test_live_subset_reloads_all_returned_words(self) -> None:
        case = MODULE.BenchCase(abi="value_blob", scenario="live_subset", state_words=8, helper_ops=4)
        text = MODULE.render_ptx(case, "sm_89")
        self.assertEqual(text.count("ld.param.b32 %r8, [ret_blob+"), case.state_words)

    def test_parse_ptxas_info(self) -> None:
        text = (
            "ptxas info    : Function properties for kernel\n"
            "    64 bytes stack frame, 16 bytes spill stores, 24 bytes spill loads\n"
            "ptxas info    : Used 31 registers, used 0 barriers, 80 bytes cmem[0], 128 bytes lmem\n"
        )
        self.assertEqual(MODULE.parse_ptxas_info(text), (31, 128, 64, 16, 24))

    def test_cli_entry_runs_smoke_case(self) -> None:
        if shutil.which("ptxas") is None or shutil.which("cuobjdump") is None:
            self.skipTest("CUDA tools are unavailable")
        with tempfile.TemporaryDirectory() as tmpdir:
            proc = subprocess.run(
                [
                    "python3",
                    "tools/run_experiment.py",
                    "--match",
                    "value_blob_input_dead_s32_o4",
                    "--out-dir",
                    tmpdir,
                ],
                cwd=MODULE_PATH.parents[1],
                check=False,
                capture_output=True,
                text=True,
            )
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main()
