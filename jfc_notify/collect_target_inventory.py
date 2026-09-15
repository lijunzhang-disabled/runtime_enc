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


def short_text(value, limit=300):
    value = str(value).strip()
    return value if len(value) <= limit else value[:limit] + "..."


def command_summary(result, lines=1):
    summary = {"status": result.get("status", "unknown")}
    if result.get("argv"):
        summary["executable"] = result["argv"][0]
    if "returncode" in result:
        summary["returncode"] = result["returncode"]
    useful = [line.strip() for line in result.get("stdout", "").splitlines()
              if any(char.isalnum() for char in line)]
    if useful:
        summary["summary"] = [short_text(line, 180) for line in useful[:lines]]
    error = result.get("stderr") or result.get("error")
    if error:
        summary["error"] = short_text(error)
    return summary


def library_kind(library):
    names = [Path(library["path"]).name]
    names += [Path(alias).name for alias in library.get("aliases", [])]
    for kind, prefix in (("urma", "liburma.so"), ("hal", "libascend_hal.so"),
                         ("runtime", "libruntime.so"), ("acl", "libascendcl.so"),
                         ("aicpu", "libaicpu_kernels.so")):
        if any(name.startswith(prefix) for name in names):
            return kind
    return "provider"


def concise_report(report):
    if report.get("format") == "concise":
        return report
    limit = 3
    machine = report.get("machine", "unknown")
    expected_elf = {"aarch64": "AArch64", "x86_64": "Advanced Micro Devices X86-64"}.get(machine)

    def priority(library):
        path = library["path"]
        parts = set(Path(path).parts)
        # Rank plausible Host libraries first, without claiming loader selection.
        stub = bool(parts & {"stub", "stubs", "devlib"})
        matches = library.get("elf", {}).get("Machine") == expected_elf
        return (stub, not matches, len(path), path)

    groups = {}
    for library in report.get("libraries", []):
        groups.setdefault(library_kind(library), {})[library["path"]] = library
    libraries = {}
    for kind, entries in sorted(groups.items()):
        candidates = []
        for library in sorted(entries.values(), key=priority)[:limit]:
            candidate = {"path": library["path"],
                         "machine": library.get("elf", {}).get("Machine", "not_inspected")}
            if kind == "urma":
                exports = library.get("required_exports")
                if exports is not None:
                    candidate["missing_exports"] = [name for name in URMA_SYMBOLS if exports.get(name) is False]
                    unchecked = [name for name in URMA_SYMBOLS if name not in exports]
                    candidate["exports_checked"] = len(URMA_SYMBOLS) - len(unchecked)
                    if unchecked:
                        candidate["unchecked_exports"] = unchecked
                else:
                    candidate["exports_checked"] = 0
            for check in ("elf_inspection", "symbol_inspection"):
                if check in library:
                    candidate[check] = command_summary(library[check], lines=0)
            candidates.append(candidate)
        libraries[kind] = {"found": len(entries), "candidates": candidates,
                           "omitted": max(0, len(entries) - limit)}

    header_groups = {}
    for header in report.get("headers", []):
        path = header["path"]
        header_groups.setdefault(Path(path).name, set()).add(path)
    headers = {name: {"found": len(paths), "paths": sorted(paths, key=lambda p: (len(p), p))[:limit],
                      "omitted": max(0, len(paths) - limit)}
               for name, paths in sorted(header_groups.items())}
    versions = []
    for entry in report.get("driver_version_files", [])[:2]:
        lines = [line.strip() for line in entry.get("text", "").splitlines() if line.strip()]
        relevant = [line for line in lines if any(word in line.lower()
                    for word in ("version", "release", "build", "package"))]
        versions.append({"path": entry["path"],
                         "summary": [short_text(line, 180) for line in (relevant or lines)[:6]]})
    nodes = report.get("device_nodes", [])
    warnings = report.get("search_warnings", [])
    return {
        "schema_version": 2, "format": "concise",
        "purpose": "Host inventory only; listed paths are candidates, not verified loaded libraries",
        "timestamp_utc": report.get("timestamp_utc"), "machine": machine,
        "kernel": report.get("kernel"),
        "driver_version": versions,
        "npu_smi": command_summary(report.get("npu_smi", {}), lines=6),
        "compiler": command_summary(report.get("compiler", {})),
        "device_nodes": {"count": len(nodes), "sample": nodes[:8]},
        "ub_sysfs": {path: {"count": len(names), "sample": names[:8]}
                     for path, names in report.get("ub_sysfs", {}).items()},
        "libraries": libraries, "headers": headers,
        "search": {"roots_checked": len(report.get("searched_roots", [])),
                   "warning_count": len(warnings), "warnings": [short_text(w) for w in warnings[:4]]},
        "functional_checks": {name: report.get(name, "NOT_TESTED") for name in (
            "host_queue_creation", "device_side_urma", "device_to_host_notification")},
    }


def write_report(report, output_path, full):
    if not full:
        report = concise_report(report)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(f"Report: {output_path.resolve()} ({output_path.stat().st_size} bytes)")
    print(f"Architecture: {report.get('machine', 'unknown')}")
    if not full:
        for kind, group in report["libraries"].items():
            print(f"{kind}: {group['found']} found, {len(group['candidates'])} shown")
    print("Report collection does not establish URMA/AICPU functionality.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True,
                        help="New JSON report path; existing files are not overwritten")
    parser.add_argument("--search-root", type=Path, action="append", default=[],
                        help="Additional installed SDK/library directory; may be repeated")
    parser.add_argument("--full", action="store_true",
                        help="Include all paths, aliases, and raw command output; default is concise")
    parser.add_argument("--from-report", type=Path,
                        help="Summarize a previously collected JSON report without probing again")
    args = parser.parse_args()
    if args.output.exists():
        parser.error(f"report already exists: {args.output}")
    if args.from_report:
        if args.full or args.search_root:
            parser.error("--from-report cannot be combined with --full or --search-root")
        try:
            report = json.loads(args.from_report.read_text())
        except (OSError, ValueError) as error:
            parser.error(str(error))
        if not isinstance(report, dict) or report.get("schema_version") not in (1, 2):
            parser.error("expected an inventory report generated by this script")
        write_report(report, args.output, full=False)
        return 0

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
    write_report(report, args.output, args.full)
    return 0  # Success means the inventory was collected, not that hardware support passed.


if __name__ == "__main__":
    sys.exit(main())
