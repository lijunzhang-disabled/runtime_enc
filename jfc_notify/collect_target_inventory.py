#!/usr/bin/env python3
"""Collect Host-side prerequisites for the JFC notification experiment.

This is an inventory, not proof of URMA or AICPU functionality. It does not load
vendor libraries, create queues, submit transfers, or reset devices.
"""

import argparse
import datetime
import fnmatch
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


LIBRARY_PATTERNS = (
    "*urma*.so*", "*udma*.so*", "libascend_hal.so*", "libruntime.so*",
    "libascendcl.so*", "libaicpu_kernels.so*",
)
HEADER_NAMES = {"urma_api.h", "urma_types.h", "urma_opcode.h", "ascend_hal.h"}
URMA_SYMBOLS = (
    "urma_init", "urma_uninit", "urma_get_device_list", "urma_free_device_list",
    "urma_get_eid_list", "urma_free_eid_list", "urma_create_context",
    "urma_delete_context", "urma_create_jfce", "urma_delete_jfce",
    "urma_create_jfc", "urma_delete_jfc", "urma_create_jfr", "urma_delete_jfr",
    "urma_create_jfs", "urma_delete_jfs", "urma_register_seg",
    "urma_unregister_seg", "urma_import_jfr", "urma_unimport_jfr",
    "urma_post_jfr_wr", "urma_post_jfs_wr", "urma_poll_jfc",
    "urma_wait_jfc", "urma_ack_jfc", "urma_rearm_jfc",
)


def command(argv, timeout=15):
    executable = shutil.which(str(argv[0]))
    if not executable:
        return {"argv": argv, "status": "not_found"}
    env = dict(os.environ, LC_ALL="C")
    try:
        result = subprocess.run(
            [executable, *argv[1:]], text=True, errors="replace",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
            env=env, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"argv": argv, "status": "timeout"}
    except OSError as error:
        return {"argv": argv, "status": "error", "error": str(error)}
    return {
        "argv": [executable, *argv[1:]], "status": "finished",
        "returncode": result.returncode,
        "stdout": result.stdout, "stderr": result.stderr,
    }


def scan(roots):
    files, errors, scanned, visited = {}, [], [], set()
    for root in roots:
        root = root.expanduser().resolve()
        if root in visited or not root.is_dir():
            continue
        visited.add(root)
        scanned.append(str(root))
        count = 0
        for folder, directories, names in os.walk(
                root, followlinks=False, onerror=lambda error: errors.append(str(error))):
            depth = len(Path(folder).relative_to(root).parts)
            directories[:] = sorted(d for d in directories if d not in {
                ".git", "__pycache__", "node_modules", "site-packages",
            }) if depth < 7 else []
            count += len(names) + len(directories)
            if count > 50000:
                errors.append(f"Search limit reached under {root}; add a narrower --search-root")
                break
            for name in names:
                if name not in HEADER_NAMES and not any(
                        fnmatch.fnmatch(name, pattern) for pattern in LIBRARY_PATTERNS):
                    continue
                candidate = Path(folder) / name
                if candidate.is_file():
                    files.setdefault(str(candidate.resolve()), []).append(str(candidate))
    return files, scanned, errors


def library_info(path, aliases):
    header = command(["readelf", "-h", path])
    details = {"path": path, "aliases": aliases}
    if header.get("returncode") == 0:
        details["elf"] = {
            line.strip().split(":", 1)[0]: line.split(":", 1)[1].strip()
            for line in header["stdout"].splitlines()
            if line.strip().startswith(("Class:", "Data:", "Machine:"))
        }
    else:
        details["elf_inspection"] = header
    if any(Path(alias).name.startswith("liburma.so") for alias in aliases):
        result = command(["readelf", "--dyn-syms", "--wide", path])
        if result.get("returncode") == 0:
            exported = set()
            for line in result["stdout"].splitlines():
                fields = line.split()
                if (len(fields) >= 8 and fields[6] != "UND"
                        and fields[4] in ("GLOBAL", "WEAK")
                        and fields[5] in ("DEFAULT", "PROTECTED")):
                    exported.add(fields[7].split("@", 1)[0])
            details["required_exports"] = {name: name in exported for name in URMA_SYMBOLS}
        else:
            details["symbol_inspection"] = result
    return details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="New JSON report path; existing files are not overwritten")
    parser.add_argument("--search-root", type=Path, action="append", default=[],
                        help="Additional installed SDK/library directory; may be repeated")
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"report already exists: {args.output}")

    roots = [Path(p) for p in (
        "/usr/local/Ascend", "/usr/local/ub", "/opt/ub", "/usr/include",
        "/usr/local/include", "/usr/lib64", "/usr/local/lib64",
        "/usr/lib/aarch64-linux-gnu", "/usr/lib/x86_64-linux-gnu",
        "/usr/local/lib",
    )]
    roots += args.search_root
    for name in ("ASCEND_HOME_PATH", "ASCEND_INSTALL_PATH", "ASCEND_TOOLKIT_HOME"):
        if os.environ.get(name):
            roots.append(Path(os.environ[name]))
    roots += [Path(p) for p in os.environ.get("LD_LIBRARY_PATH", "").split(":") if p]
    for path in Path("/usr/local/Ascend").glob("*/latest"):
        roots.append(path)

    loader = command(["ldconfig", "-p"])
    if loader.get("returncode") == 0:
        for line in loader["stdout"].splitlines():
            if "=>" in line:
                name, path = line.split("=>", 1)
                if any(fnmatch.fnmatch(name.strip().split()[0], pattern)
                       for pattern in LIBRARY_PATTERNS):
                    roots.append(Path(path.strip()).parent)

    found, searched, errors = scan(roots)
    libraries = [library_info(path, aliases) for path, aliases in sorted(found.items())
                 if Path(path).name not in HEADER_NAMES]
    headers = [{"path": path, "aliases": aliases} for path, aliases in sorted(found.items())
               if Path(path).name in HEADER_NAMES]
    version_files = []
    for path in (Path("/usr/local/Ascend/driver/version.info"),):
        if path.is_file():
            try:
                version_files.append({"path": str(path), "text": path.read_text(errors="replace")[:16384]})
            except OSError as error:
                errors.append(str(error))
    npu_tool = shutil.which("npu-smi") or "/usr/local/Ascend/driver/tools/npu-smi"
    report = {
        "schema_version": 1,
        "purpose": "Host inventory only; URMA/AICPU functionality has not been tested",
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "machine": platform.machine(), "kernel": platform.release(),
        "npu_smi": command([npu_tool, "info"]),
        "compiler": command(["c++", "--version"]),
        "device_nodes": sorted(str(p) for p in Path("/dev").glob("davinci*")),
        "ub_sysfs": {str(root): sorted(p.name for p in root.iterdir())
                     for root in (Path("/sys/class/ubcore"), Path("/sys/class/uburma"),
                                  Path("/sys/class/udma")) if root.is_dir()},
        "driver_version_files": version_files,
        "searched_roots": searched, "search_warnings": errors,
        "libraries": libraries, "headers": headers,
        "loader_cache_status": {key: value for key, value in loader.items()
                                if key not in ("stdout", "stderr")},
        "device_side_urma": "NOT_TESTED",
        "host_queue_creation": "NOT_TESTED",
        "device_to_host_notification": "NOT_TESTED",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(f"Report: {args.output.resolve()}")
    print(f"Architecture: {report['machine']}")
    print(f"NPU inventory: {report['npu_smi']['status']}, rc={report['npu_smi'].get('returncode', 'n/a')}")
    print(f"URMA API headers: {sum(Path(h['path']).name == 'urma_api.h' for h in headers)}")
    for library in libraries:
        if "required_exports" in library:
            missing = [name for name, present in library["required_exports"].items() if not present]
            print(f"URMA library: {library['path']}")
            print("Missing inspected exports: " + (", ".join(missing) if missing else "none"))
    print(f"Discovered libraries: {len(libraries)}; search warnings: {len(errors)}")
    print("Host queues / AICPU URMA / device-to-Host notification: NOT TESTED")
    return 0  # Success means the inventory was collected, not that hardware support passed.


if __name__ == "__main__":
    sys.exit(main())
