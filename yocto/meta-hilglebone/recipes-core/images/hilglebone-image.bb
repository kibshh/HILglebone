SUMMARY = "HILglebone HIL orchestrator image for BeagleBone Black"
DESCRIPTION = "Minimal Yocto image with the HILglebone Python orchestrator, \
its runtime dependencies, and (in dev builds) developer conveniences."
LICENSE = "MIT"

# Inherit a small base image. core-image-minimal gives us a bootable system
# with BusyBox userspace, kernel, and init. Everything else we add explicitly
# via IMAGE_INSTALL.
inherit core-image

# Packages that always ship in the HILglebone image.
#
# Note: the orchestrator's Python runtime dependencies (python3, pyserial,
# pyyaml) are NOT listed here. They are declared as RDEPENDS in
# hilglebone-orchestrator.bb, which is the correct place for "this package
# needs these to run." BitBake pulls them in automatically.
#
# Spacing convention used throughout this recipe:
#   ":append = ' foo bar'"   -- ALWAYS use ":append" for list-style variables,
#                               and ALWAYS include a leading space inside the
#                               string. ":append" does pure string concatenation
#                               with NO automatic separator, so the leading
#                               space is what keeps tokens from gluing together.
# Avoid the "+=" operator: it inserts a space automatically, which would force
# us to remember a different rule for each operator.
#
#   hilglebone-orchestrator -- our Python application + systemd unit
#   hilglebone-uart-overlay -- device-tree overlay enabling the UART pinmux
#   kernel-modules          -- pulled in for completeness; the overlay relies on
#                              standard tty drivers being available
IMAGE_INSTALL:append = " hilglebone-orchestrator hilglebone-uart-overlay kernel-modules"

# Development-only extras. Gated by HILGLEBONE_DEV (defined in local.conf).
# Listing them here, instead of in local.conf, keeps them scoped to *this*
# image -- a stray `bitbake core-image-minimal` from the same build dir will
# not pick them up.
#
#   openssh-sftp-server -- file transfer for pushing code onto the board
#   htop                -- interactive process inspection
#   usb-gadget-net      -- USB Ethernet gadget (usb0 @ 192.168.7.2), lets
#                          the laptop SSH into the BBB over the same
#                          micro-USB cable that provides power. Pure
#                          developer convenience; never ship this in prod.
IMAGE_INSTALL:append = "${@' openssh-sftp-server htop usb-gadget-net' if d.getVar('HILGLEBONE_DEV') == '1' else ''}"

# Image-level features. Same gating logic as above.
#
#   debug-tweaks        -- blank root password, easy serial/ssh login
#   ssh-server-openssh  -- full OpenSSH daemon (not just sftp-server)
#   tools-debug         -- gdb, strace, ltrace
IMAGE_FEATURES:append = "${@' debug-tweaks ssh-server-openssh tools-debug' if d.getVar('HILGLEBONE_DEV') == '1' else ''}"

# Place our UART overlay on the FAT boot partition so U-Boot can load it
# via the FDTOVERLAYS directive in extlinux.conf. The overlay recipe's
# do_deploy task puts the .dtbo into DEPLOY_DIR_IMAGE; this line tells wic
# to copy it onto the partition.
IMAGE_BOOT_FILES:append = " hilglebone-uart-overlay.dtbo"

# Reasonable rootfs ceiling. BeagleBone Black eMMC is 4 GiB, SD cards are
# usually 8+ GiB. 512 MiB leaves plenty of headroom for logs and updates.
IMAGE_ROOTFS_EXTRA_SPACE = "524288"
