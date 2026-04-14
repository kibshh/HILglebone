SUMMARY = "USB Ethernet gadget setup for BeagleBone Black"
DESCRIPTION = "Loads the g_ether kernel module at boot and configures \
the usb0 interface with a static IP (192.168.7.2/24), so the BBB \
exposes a virtual Ethernet device to a connected host PC. This \
allows SSH over the same micro-USB cable that supplies power, \
without needing a separate serial adapter or RJ45 connection."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://g_ether.conf \
    file://g_ether.options \
    file://80-usb0.network \
"

S = "${WORKDIR}"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    # Kernel-module autoload config. systemd-modules-load.service will
    # pick this up at boot and modprobe g_ether, which creates /dev/usb0
    # once the USB OTG port enters peripheral mode (cable plugged into
    # a host).
    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/g_ether.conf \
        ${D}${sysconfdir}/modules-load.d/g_ether.conf

    # Pin the gadget's MAC addresses. Installed to /etc/modprobe.d/
    # (NOT modules-load.d -- modprobe reads options from here when
    # systemd-modules-load.service triggers the modprobe for g_ether).
    # Stable MACs mean stable host-side enx<mac> interface names, so
    # bbb-usb-connect.sh can find the right interface deterministically.
    install -d ${D}${sysconfdir}/modprobe.d
    install -m 0644 ${WORKDIR}/g_ether.options \
        ${D}${sysconfdir}/modprobe.d/g_ether.conf

    # systemd-networkd config for the usb0 interface. Assigns the
    # BBB-side IP. A matching config on the host is documented in
    # the .network file itself.
    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/80-usb0.network \
        ${D}${sysconfdir}/systemd/network/80-usb0.network
}

FILES:${PN} = " \
    ${sysconfdir}/modules-load.d/g_ether.conf \
    ${sysconfdir}/modprobe.d/g_ether.conf \
    ${sysconfdir}/systemd/network/80-usb0.network \
"

# Runtime deps:
#   kernel-module-g-ether -- the actual USB Ethernet gadget driver.
#                            Must be built as a module by the kernel
#                            recipe for this to work.
#   systemd               -- both modules-load.d and systemd-networkd
#                            are systemd-specific. If the distro ever
#                            switches to sysvinit these configs would
#                            need to be reworked.
RDEPENDS:${PN} = "kernel-module-g-ether systemd"
