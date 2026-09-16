#!/usr/bin/env python3
"""Local CPU/mocked checks; never load the installed driver or run on an NPU."""
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PROBE = HERE.parent
ROOT = PROBE.parent.parent
AICPU = Path(os.environ.get("AICPU_ROOT", ROOT / "AscendCCv2-AICPU"))
RUNTIME = Path(os.environ.get("RUNTIME_ROOT", ROOT / "AscendCCv2-Runtime"))


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed ({result.returncode})\n{result.stdout}\n{result.stderr}")
    return result.stdout


def main():
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory(prefix="aicpu-urma-check-") as tmp:
        build = Path(tmp)
        runtime_includes = [f"-I{RUNTIME / 'pkg_inc/runtime'}", f"-I{RUNTIME / 'pkg_inc'}"]
        protocol_dir = AICPU / "ms_kernels/src/secure_dma"
        common = [cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
        # Deliberately HOST shape (portable fences and OpenSSL cipher backend): no
        # device barrier/ISA validation is claimed.
        # The normal AICPU CMake retains its SECURE_DMA_DEVICE_BUILD / AArch64 guard.
        fragment = (AICPU / "ms_kernels/secure_dma_v2.cmake").read_text()
        source_list = fragment.split("set(SECURE_DMA_V2_SOURCES", 1)[1].split(")", 1)[0]
        sources = [AICPU / "ms_kernels" / s for s in re.findall(r'\$\{CMAKE_CURRENT_LIST_DIR\}/([^"]+)', source_list)]
        isa = ["-maes", "-msse2"] if platform.machine() == "x86_64" else ["-march=armv8-a+crypto"]
        includes = [f"-I{AICPU / s}" for s in ("stub/aicpu_framework", "ms_kernels/inc", "ms_kernels/src")]
        objects = []
        for source in sources:
            obj = build / (source.stem + ".o")
            run(common + isa + includes + ["-pthread", "-c", source, "-o", obj])
            objects.append(obj)
        for name in ("fixed_operator", "small_no_key", "small_operator", "operator"):
            test = AICPU / "test" / ("test_secure_dma_" + name + ".cc")
            binary = build / name
            extra = [AICPU / "test/secure_dma_test_key_binding.cc"] if name == "operator" else []
            run(common + isa + includes + ["-pthread", test] + extra + objects + ["-lcrypto", "-o", binary])
            output = run([binary])
            print(f"local_{name}=PASS {output.strip().splitlines()[-1]}")

        mock = build / "libruntime_mock.so"
        run(common + runtime_includes + [f"-I{protocol_dir}", "-fPIC", "-shared",
            HERE / "fake_runtime.cpp", "-o", mock])
        launcher = build / "aicpu_urma_probe"
        env = dict(os.environ, RUNTIME_INCLUDE_DIR=str(RUNTIME / "pkg_inc/runtime"),
                   RUNTIME_LIBRARY=str(mock), PROBE_PROTOCOL_DIR=str(protocol_dir),
                   PROBE_OUTPUT=str(launcher), CXX=cxx)
        env.pop("SECURE_AICPU_SO_PATH", None)
        run(["bash", PROBE / "build_aicpu_probe.sh"], env=env)
        cases = {"pass": 0, "allow_load": 0, "unavailable": 3, "partial": 3}
        cases.update({name: 1 for name in (
            "close_fail", "bad_cookie", "bad_magic", "bad_done", "bad_count", "bad_mask",
            "bad_arch", "bad_status", "bad_source", "bad_text", "set_fail", "stream_fail",
            "malloc_fail", "write_fail", "launch_fail", "sync_fail", "read_fail", "free_fail", "destroy_fail")})
        for name, code in cases.items():
            env["MOCK_CASE"] = name
            args = [str(launcher), "--device", "0"] + (["--allow-load"] if name == "allow_load" else [])
            result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == code, (name, result.returncode, result.stdout, result.stderr)
            assert "graph_replay=NOT_TESTED" in result.stdout, (name, result.stdout)
            if name == "sync_fail":
                assert "cleanup=DEFERRED_TO_PROCESS_EXIT" in result.stdout
                assert "mock=free" not in result.stdout and "mock=destroy" not in result.stdout
            if code == 0:
                assert "aicpu_urma_symbols=PASS" in result.stdout and f"loaded_runtime={mock}" in result.stdout
            if name == "partial":
                assert "missing_symbols=urma_poll_jfc\n" in result.stdout
        cli_cases = [("--device", "-1"), ("--device", "abc"), ("--device",), ("--timeout-ms", "0"),
                     ("--timeout-ms", "30001"), ("--device", "--timeout-ms", "100"),
                     ("--unknown",), ("--help",)]
        for args in cli_cases:
            result = subprocess.run([str(launcher), *args], env=env, capture_output=True, text=True, timeout=10)
            assert result.returncode == (0 if args == ("--help",) else 2), (args, result)
            assert "step=set_device" not in result.stdout
        env["SECURE_AICPU_SO_PATH"] = "/mock/side-loaded.so"
        result = subprocess.run([str(launcher)], env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode == 2 and "step=set_device" not in result.stdout
        print(f"local_launcher=PASS cases={len(cases) + len(cli_cases) + 1} provider=mock")
        print("scope=CPU_logic_mock_loader_mock_runtime AICPU_CI_build=NOT_TESTED target_execution=NOT_TESTED")


if __name__ == "__main__":
    main()
