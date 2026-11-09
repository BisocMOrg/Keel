#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
"""Keep the packaging metadata consistent with the library it ships.

Checks, all against single sources in the tree:
1. the version in debian/changelog, CHANGELOG.md and the Doxyfile equals
   KEEL_VERSION_* in keel.h;
2. debian/control declares the board packages libkeel<major>, libkeel-dev,
   keel-common (arch all) and keel-tools, and the camera packages
   libkeel-camera<major>, libkeel-camera-dev and keel-camera, with the runtime
   package tied to keel-common, the camera library depending on libcamera's
   IPA modules, the board packages free of the camera, and the camera tool
   and development files tied to the camera library;
3. debian/libkeel<major>.symbols lists exactly symbols.txt under the version
   node of cmake/keel.map, for the SONAME libkeel.so.<major>, and
   debian/libkeel-camera<major>.symbols lists camera_symbols.txt under the
   node of cmake/keel-camera.map, for libkeel-camera.so.<major>;
4. every file under packaging/common is installed by cmake/packaging.cmake
   (into the keel-common package) or shipped as a keel-common example;
5. the udev rule never forces an assignment (no ":="), creates the
   /dev/keel/gpiochip link that keel_r3 and keel-doctor probe, the rtprio limit and
   the example unit grant KEEL_RT_PRIORITY, the sysusers file creates the
   keel group and modules-load names i2c-dev;
6. install.sh fetches from the Homepage of debian/control and installs the
   packages of debian/control, plus the binding packages that the same
   release carries (python3-keel, built from software/bindings/python), and
   leaves out exactly the camera packages with --no-camera.

Usage: check_package_metadata.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
# Released beside the libkeel packages and installed by install.sh on request.
BINDING_PACKAGES = {"python3-keel"}
problems = []


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as handle:
        return handle.read()


def expect(condition, message):
    if not condition:
        problems.append(message)


def header_values():
    text = read("include", "keel", "keel.h")
    version = [re.search(r"#define KEEL_VERSION_%s (\d+)" % part, text).group(1)
               for part in ("MAJOR", "MINOR", "PATCH")]
    rt_priority = int(re.search(r"KEEL_RT_PRIORITY = (\d+)", text).group(1))
    camera = read("include", "keel", "camera.h")
    camera_major = int(re.search(r"#define KEEL_CAMERA_VERSION_MAJOR (\d+)", camera).group(1))
    return ".".join(version), int(version[0]), rt_priority, camera_major


def control_paragraphs():
    text = "\n".join(line for line in read("debian", "control").split("\n")
                     if not line.startswith("#"))
    paragraphs = []
    for chunk in re.split(r"\n\s*\n", text.strip()):
        fields, key = {}, None
        for line in chunk.split("\n"):
            if line[:1].isspace() and key:
                fields[key] += "\n" + line.strip()
            else:
                key, _, value = line.partition(":")
                fields[key] = value.strip()
        paragraphs.append(fields)
    return paragraphs[0], {p["Package"]: p for p in paragraphs[1:]}


def check_versions(version):
    changelog = [line for line in read("debian", "changelog").split("\n")
                 if line and not line.startswith("#")]
    match = re.match(r"libkeel \(([^)]+)\)", changelog[0])
    expect(match and match.group(1) == version,
           "debian/changelog starts with %r, keel.h says %s" % (changelog[0], version))
    expect("## [%s]" % version in read("CHANGELOG.md"),
           "CHANGELOG.md has no '## [%s]' section" % version)
    number = re.search(r"^PROJECT_NUMBER\s*=\s*(\S+)", read("Doxyfile"), re.M)
    expect(number and number.group(1) == version,
           "Doxyfile PROJECT_NUMBER is %s, keel.h says %s" % (number and number.group(1), version))


def check_control(major, camera_major):
    source, packages = control_paragraphs()
    runtime = "libkeel%d" % major
    camera = "libkeel-camera%d" % camera_major
    expected = {runtime: "any", "libkeel-dev": "any", "keel-common": "all", "keel-tools": "any",
                camera: "any", "libkeel-camera-dev": "any", "keel-camera": "any"}
    expect(sorted(packages) == sorted(expected),
           "debian/control packages %s, expected %s" % (sorted(packages), sorted(expected)))
    for name, arch in expected.items():
        if name in packages:
            expect(packages[name].get("Architecture") == arch,
                   "%s: Architecture %s, expected %s" % (name, packages[name].get("Architecture"), arch))
    if runtime in packages:
        expect("keel-common (= ${source:Version})" in packages[runtime].get("Depends", ""),
               "%s must depend on keel-common (= ${source:Version})" % runtime)
    if camera in packages:
        expect("libcamera-ipa" in packages[camera].get("Depends", ""),
               "%s must depend on libcamera-ipa (libcamera loads it at run time)" % camera)
    for name in ("libkeel-dev", "keel-tools"):
        if name in packages:
            expect("camera" not in packages[name].get("Depends", ""),
                   "%s must not depend on the camera packages, so that it installs without libcamera" % name)
    for name in ("libkeel-camera-dev", "keel-camera"):
        if name in packages:
            expect("%s (= ${binary:Version})" % camera in packages[name].get("Depends", ""),
                   "%s must depend on %s (= ${binary:Version})" % (name, camera))
    return source, packages


def check_symbols(library, package, symbols_file, map_file):
    symbols = [line.strip() for line in read("tests", "abi", symbols_file).split("\n")
               if line.strip() and not line.startswith("#")]
    node = re.search(r"^(KEEL_[A-Z_]*\d+\.\d+) \{", read("cmake", map_file), re.M).group(1)
    lines = [line for line in read("debian", "%s.symbols" % package).split("\n")
             if line and not line.startswith("#")]
    expect(lines[0] == "%s %s #MINVER#" % (library, package),
           "debian/%s.symbols starts with %r" % (package, lines[0]))
    listed = {}
    for line in lines[1:]:
        match = re.match(r"^ (\S+)@(\S+) (\S+)$", line)
        if match:
            listed[match.group(1)] = match.group(2)
    expected = set(symbols) | {node}
    expect(set(listed) == expected,
           "debian/%s.symbols differs from %s: missing %s, extra %s"
           % (package, symbols_file, sorted(expected - set(listed)), sorted(set(listed) - expected)))
    wrong = sorted(name for name, version in listed.items() if version != node)
    expect(not wrong, "debian/%s.symbols outside %s: %s" % (package, node, wrong))


def check_common_files():
    cmake = read("cmake", "packaging.cmake")
    directories = dict(re.findall(r'set\((KEEL_\w+_DIR) "([^"]+)"', cmake))
    installed = {}
    for source, destination in re.findall(
            r'install\(FILES "\$\{_keel_common\}/([^"]+)"\s+DESTINATION "\$\{(KEEL_\w+_DIR)\}"', cmake):
        installed[source] = directories[destination].lstrip("/") + "/" + os.path.basename(source)
    examples = [line.strip() for line in read("debian", "keel-common.examples").split("\n")
                if line.strip() and not line.startswith("#")]
    common = os.path.join(ROOT, "packaging", "common")
    for directory, _, files in os.walk(common):
        for name in files:
            relative = os.path.relpath(os.path.join(directory, name), common).replace(os.sep, "/")
            expect(relative in installed or "packaging/common/" + relative in examples,
                   "packaging/common/%s is neither installed nor an example" % relative)
    listed = [line.strip() for line in read("debian", "keel-common.install").split("\n")
              if line.strip() and not line.startswith("#")]
    expect(sorted(listed) == sorted(installed.values()),
           "debian/keel-common.install %s differs from packaging.cmake %s"
           % (sorted(listed), sorted(installed.values())))


def check_common_contents(rt_priority):
    rules = read("packaging", "common", "udev", "99-keel.rules")
    active = [line for line in rules.split("\n") if line and not line.startswith("#")]
    expect(not any(":=" in line for line in active), "99-keel.rules forces an assignment (:=)")
    expect(any('GROUP="keel"' in line for line in active), "99-keel.rules never falls back to keel")
    # Both gpio rules (distribution group or keel) create the link that the
    # library and keel-doctor probe after their /dev/gpiochip* scan.
    gpio_rules = [line for line in active if 'SUBSYSTEM=="gpio"' in line]
    link = 'SYMLINK+="keel/gpiochip"'
    expect(gpio_rules and all(link in line for line in gpio_rules),
           "a gpio rule in 99-keel.rules lacks %s" % link)
    for path in (("src", "backend", "keel_r3", "r3_gpio.c"), ("tools", "keel-doctor.c")):
        expect('"/dev/keel/gpiochip"' in read(*path),
               "%s does not probe /dev/keel/gpiochip, the link 99-keel.rules creates"
               % "/".join(path))
    limits = read("packaging", "common", "limits.d", "90-keel-rtprio.conf")
    expect(re.search(r"^@keel\s+-\s+rtprio\s+%d$" % rt_priority, limits, re.M),
           "90-keel-rtprio.conf does not grant rtprio %d to @keel" % rt_priority)
    unit = read("packaging", "common", "systemd", "keel-app.service")
    expect(re.search(r"^LimitRTPRIO=%d$" % rt_priority, unit, re.M),
           "keel-app.service does not set LimitRTPRIO=%d" % rt_priority)
    expect(re.search(r"^g keel -$", read("packaging", "common", "sysusers.d", "keel.conf"), re.M),
           "sysusers.d/keel.conf does not create the keel group")
    expect(re.search(r"^i2c-dev$", read("packaging", "common", "modules-load.d", "keel.conf"), re.M),
           "modules-load.d/keel.conf does not load i2c-dev")


def check_install_script(source, packages):
    script = read("install.sh")
    url = re.search(r'^REPOSITORY_URL="([^"]+)"', script, re.M)
    expect(url and url.group(1) == source.get("Homepage"),
           "install.sh REPOSITORY_URL differs from the Homepage of debian/control")
    board, camera = set(), set()
    for variable, value in re.findall(r'^\s*(packages|camera_packages)="([^"]*)"', script, re.M):
        names = {entry.split(":")[0] for entry in value.split() if not entry.startswith("$")}
        (camera if variable == "camera_packages" else board).update(names)
    expect(board | camera == set(packages) | BINDING_PACKAGES, "install.sh installs %s, debian/control builds %s"
           " and the bindings add %s" % (sorted(board | camera), sorted(packages), sorted(BINDING_PACKAGES)))
    expect(camera == {name for name in packages if "camera" in name},
           "install.sh's camera packages (left out by --no-camera) are %s" % sorted(camera))


def main():
    version, major, rt_priority, camera_major = header_values()
    check_versions(version)
    source, packages = check_control(major, camera_major)
    check_symbols("libkeel.so.%d" % major, "libkeel%d" % major, "symbols.txt", "keel.map")
    check_symbols("libkeel-camera.so.%d" % camera_major, "libkeel-camera%d" % camera_major,
                  "camera_symbols.txt", "keel-camera.map")
    check_common_files()
    check_common_contents(rt_priority)
    check_install_script(source, packages)
    for problem in problems:
        print("FAIL  " + problem)
    if problems:
        return 1
    print("check_package_metadata: OK: version %s, %d packages, symbols, keel-common files, "
          "install.sh" % (version, len(packages)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
