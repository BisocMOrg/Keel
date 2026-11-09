# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# System integration installed with KEEL_PACKAGING=ON: the files of the
# keel-common package and keel-setup. The Debian build (debian/rules) turns
# the option on and splits the install tree into packages, so a source
# install on the robot and the .debs put the same files in the same places:
#
#   udev rule        99-keel.rules   gpiochip and i2c-1 access (gpio/i2c or keel group)
#   modules-load.d   keel.conf       loads i2c-dev at boot (/dev/i2c-1)
#   sysusers.d       keel.conf       the keel system group
#   limits.d         90-keel-rtprio  rtprio 40 for the keel group
#   bin              keel-setup      groups and boot configuration
#
# The system directories are absolute: udev, systemd and PAM read only these
# locations, whatever CMAKE_INSTALL_PREFIX is. Stage an install with DESTDIR.
# After a source install, run "sudo systemd-sysusers" and
# "sudo udevadm control --reload" once (the package's postinst does this).

set(KEEL_UDEV_RULES_DIR "/usr/lib/udev/rules.d" CACHE PATH "Directory for 99-keel.rules")
set(KEEL_MODULES_LOAD_DIR "/usr/lib/modules-load.d" CACHE PATH "Directory for the i2c-dev modules-load file")
set(KEEL_SYSUSERS_DIR "/usr/lib/sysusers.d" CACHE PATH "Directory for the keel sysusers file")
set(KEEL_LIMITS_DIR "/etc/security/limits.d" CACHE PATH "Directory for the rtprio limit")

set(_keel_common "${PROJECT_SOURCE_DIR}/packaging/common")
install(FILES "${_keel_common}/udev/99-keel.rules"
        DESTINATION "${KEEL_UDEV_RULES_DIR}" COMPONENT keel_common)
install(FILES "${_keel_common}/modules-load.d/keel.conf"
        DESTINATION "${KEEL_MODULES_LOAD_DIR}" COMPONENT keel_common)
install(FILES "${_keel_common}/sysusers.d/keel.conf"
        DESTINATION "${KEEL_SYSUSERS_DIR}" COMPONENT keel_common)
install(FILES "${_keel_common}/limits.d/90-keel-rtprio.conf"
        DESTINATION "${KEEL_LIMITS_DIR}" COMPONENT keel_common)
install(PROGRAMS "${PROJECT_SOURCE_DIR}/scripts/keel-setup"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT keel_tools)
