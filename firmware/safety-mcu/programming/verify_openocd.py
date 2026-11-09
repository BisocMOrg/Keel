#!/usr/bin/env python3
"""Record/check a pinned OpenOCD build; capability checks never call init."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

COMMIT = "acff23ffd100479d6481a1155123774b79a990b9"
REPOSITORY = "https://github.com/raspberrypi/openocd.git"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def script_digest(root):
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def capabilities(binary, scripts, config):
    if not (scripts / "target/stm32c0x.cfg").is_file():
        raise ValueError("missing STM32C0 target script")
    result = subprocess.run([str(binary), "-s", str(scripts), "-c", "noinit",
                             "-c", "adapter list", "-c", "shutdown"],
                            capture_output=True, text=True, timeout=20)
    if result.returncode or "linuxgpiod" not in result.stdout + result.stderr:
        raise ValueError("OpenOCD linuxgpiod capability check failed")
    result = subprocess.run([str(binary), "-s", str(scripts), "-c", "noinit",
                             "-c", "set KEEL_GPIOCHIP 0", "-f", str(config),
                             "-c", 'echo "KEEL: CONFIG OK"', "-c", "shutdown"],
                            capture_output=True, text=True, timeout=20)
    if result.returncode or "KEEL: CONFIG OK" not in result.stdout + result.stderr:
        raise ValueError("OpenOCD board/STM32C0 command capability check failed:\n" +
                         result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("record", "verify"))
    parser.add_argument("prefix", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("openocd_keel.cfg"))
    args = parser.parse_args()
    prefix = args.prefix.resolve()
    binary = prefix / "bin/openocd"
    scripts = prefix / "share/openocd/scripts"
    manifest = prefix / "share/keel/openocd-build.json"
    actual = {"repository": REPOSITORY, "commit": COMMIT,
              "binary_sha256": sha256(binary), "scripts_sha256": script_digest(scripts)}
    if args.mode == "record":
        if not args.source:
            raise ValueError("record requires --source")
        commit = subprocess.check_output(["git", "-C", str(args.source), "rev-parse", "HEAD"], text=True).strip()
        if commit != COMMIT:
            raise ValueError("source is not the pinned commit")
        subprocess.run(["git", "-C", str(args.source), "diff", "--quiet", "HEAD", "--"], check=True)
        submodules = subprocess.check_output(["git", "-C", str(args.source), "submodule", "status", "jimtcl"], text=True)
        if not submodules.startswith(" "):
            raise ValueError("jimtcl submodule is not at its pinned revision")
        actual["jimtcl"] = submodules.strip()
        capabilities(binary, scripts, args.config)
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(json.dumps(actual, indent=2) + "\n", encoding="utf-8")
    else:
        recorded = json.loads(manifest.read_text(encoding="utf-8"))
        if any(recorded.get(key) != value for key, value in actual.items()):
            raise ValueError("OpenOCD build manifest, binary or scripts do not match")
        capabilities(binary, scripts, args.config)
    print(f"Verified pinned OpenOCD {COMMIT}; linuxgpiod and STM32C0 config; no hardware initialized")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
