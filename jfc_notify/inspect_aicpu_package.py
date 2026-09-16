#!/usr/bin/env python3
"""Print a concise, read-only AICPU package report; never load or install a library.

Recognizes ordinary tar/gzip/bzip2/xz archives and an archive payload at the
8192 + 256 byte offset used by the source-reference TSD CMS package reader.
Recognizing a payload does not validate the package's signature or deployment.
Only selected regular ELF members are copied to temporary files for readelf.
"""

import argparse
import fnmatch
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile


PAYLOAD_OFFSETS = (0, 8192 + 256)
LIBRARY_PATTERNS = ("libaicpu_kernels.so*", "libsecure_memcpy_aicpu_kernel.so*")
MAX_LIBRARIES = 4
MAX_ELF_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_MEMBERS = 50000


def short(value, limit=240):
    value = " ".join(str(value).split())
    return value if len(value) <= limit else value[:limit] + "..."


def archive_format(data):
    if data.startswith(b"\x1f\x8b\x08"):
        return "gzip"
    if data.startswith(b"BZh"):
        return "bzip2"
    if data.startswith(b"\xfd7zXZ\x00"):
        return "xz"
    if data[257:262] == b"ustar":
        return "tar"
    return None


def inspect_elf(path):
    readelf = shutil.which("readelf")
    if not readelf:
        print("  elf_inspection=NOT_TESTED reason=readelf_not_found SecureDma=UNKNOWN")
        return False
    try:
        result = subprocess.run(
            [readelf, "-h", "-d", "--dyn-syms", "--wide", str(path)],
            text=True, errors="replace", capture_output=True, check=False,
            timeout=30, env=dict(os.environ, LC_ALL="C"),
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"  elf_inspection=FAILED error={short(error)} SecureDma=UNKNOWN")
        return False
    if result.returncode:
        print(f"  elf_inspection=FAILED rc={result.returncode} "
              f"error={short(result.stderr)} SecureDma=UNKNOWN")
        return False
    machine, dependencies, exported = "unknown", [], False
    for line in result.stdout.splitlines():
        if line.strip().startswith("Machine:"):
            machine = line.split(":", 1)[1].strip()
        if "(NEEDED)" in line:
            match = re.search(r"\[([^]]+)\]", line)
            if match:
                dependencies.append(match.group(1))
        fields = line.split()
        if (len(fields) >= 8 and fields[6] != "UND"
                and fields[4] in ("GLOBAL", "WEAK")
                and fields[5] in ("DEFAULT", "PROTECTED")
                and fields[7].split("@", 1)[0] == "SecureDma"):
            exported = True
    print(f"  machine={machine} SecureDma={'EXPORTED' if exported else 'NOT_EXPORTED'}")
    print(f"  needed={','.join(dependencies[:12]) or '(none)'} "
          f"omitted={max(0, len(dependencies) - 12)}")
    return True


def inspect_package(path):
    print(f"package={path}")
    try:
        print(f"  resolved={path.resolve(strict=True)} size={path.stat().st_size}")
        with path.open("rb") as source:
            prefix = source.read(max(PAYLOAD_OFFSETS) + 512)
            detected = [(offset, archive_format(prefix[offset:])) for offset in PAYLOAD_OFFSETS]
            detected = [(offset, kind) for offset, kind in detected if kind]
            if not detected:
                print("  format=UNKNOWN")
                for offset in PAYLOAD_OFFSETS:
                    print(f"  bytes_at_{offset}={prefix[offset:offset + 16].hex() or '(EOF)'}")
                return False
            offset, kind = detected[0]
            print(f"  format={kind} payload_offset={offset} signature_verification=NOT_TESTED")
            source.seek(offset)
            libraries, inspected, versions = 0, 0, 0
            complete, ok = True, True
            # Stream archive entries. Do not use extract()/extractall(), or follow
            # archived paths/symlinks into the target filesystem.
            with tarfile.open(fileobj=source, mode="r|*") as archive:
                for count, member in enumerate(archive, 1):
                    if count > MAX_MEMBERS or member.offset_data + member.size > MAX_ARCHIVE_BYTES:
                        print("  scan=INCOMPLETE reason=archive_scan_limit")
                        complete = False
                        break
                    name = PurePosixPath(member.name).name
                    if any(fnmatch.fnmatch(name, pattern) for pattern in LIBRARY_PATTERNS):
                        libraries += 1
                        if libraries > MAX_LIBRARIES:
                            continue
                        print(f"  library_member={short(member.name, 500)} bytes={member.size}")
                        if not member.isfile():
                            print("  elf_inspection=NOT_TESTED reason=not_a_regular_file")
                            continue
                        if member.size > MAX_ELF_BYTES:
                            print("  elf_inspection=NOT_TESTED reason=member_size_limit")
                            ok = False
                            continue
                        with archive.extractfile(member) as data, tempfile.TemporaryDirectory(
                                prefix="jfc-aicpu-inspect-") as directory:
                            elf = Path(directory) / "kernel.so"
                            with elf.open("wb") as output:
                                shutil.copyfileobj(data, output, length=1024 * 1024)
                            inspected += 1
                            if not inspect_elf(elf):
                                ok = False
                    elif name == "version.info" and member.isfile() and versions < 2:
                        versions += 1
                        with archive.extractfile(member) as data:
                            lines = data.read(4096).decode("utf-8", errors="replace").splitlines()
                        useful = [line for line in lines if line.strip()]
                        print(f"  version_member={short(member.name, 500)}")
                        print("  version=" + " | ".join(short(line, 100) for line in useful[:4]))
            print(f"  library_members={libraries} elf_inspections_attempted={inspected} "
                  f"omitted={max(0, libraries - MAX_LIBRARIES)} scan={'COMPLETE' if complete else 'INCOMPLETE'}")
            return complete and ok
    except (OSError, EOFError, tarfile.TarError) as error:
        print(f"  inspection=FAILED error={short(error)}")
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, action="append",
                        help="Inspect this package; may be repeated. Default: both Ascend AICPU "
                             "packages under ASCEND_AICPU_PATH (or /usr/local/Ascend).")
    args = parser.parse_args()
    print("probe=aicpu_package_inspection version=1")
    for name in ("SECURE_AICPU_SO_PATH", "ASCEND_AICPU_PATH"):
        print(f"{name}={os.environ.get(name, '<unset>')}")
    root = Path(os.environ.get("ASCEND_AICPU_PATH") or "/usr/local/Ascend")
    print(f"package_root_resolved={root.resolve()}")
    packages = args.package or [root / "opp/Ascend/aicpu" / name for name in (
        "Ascend-aicpu_syskernels.tar.gz", "Ascend-aicpu_extend_syskernels.tar.gz")]
    ok = True
    for path in packages:
        if not inspect_package(path.expanduser()):
            ok = False
    print("scope=static_package_inspection deployed_binary=UNCONFIRMED device_urma=NOT_TESTED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
