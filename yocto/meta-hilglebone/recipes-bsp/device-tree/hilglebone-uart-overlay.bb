SUMMARY = "Device-tree overlay enabling UART1 on the BeagleBone Black expansion header"
DESCRIPTION = "Compiles a .dts overlay (.dtbo) that turns on UART1 \
(P9.24 = TX, P9.26 = RX) so the HILglebone orchestrator can talk to the \
STM32 emulator over /dev/ttyS1."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# Restrict this recipe to BeagleBone-class machines. If MACHINE is something
# else, BitBake will skip the recipe instead of trying to compile a useless
# overlay.
COMPATIBLE_MACHINE = "beaglebone"

SRC_URI = "file://hilglebone-uart-overlay.dts"

S = "${WORKDIR}"

# We need the device-tree compiler at build time. dtc-native pulls in a
# host-side dtc that runs during do_compile.
DEPENDS = "dtc-native"

do_compile() {
    # -@ keeps symbol information needed for runtime overlay application.
    # -H epapr / -O dtb produces the binary overlay format the kernel loads.
    dtc -@ -I dts -O dtb -o hilglebone-uart-overlay.dtbo \
        ${WORKDIR}/hilglebone-uart-overlay.dts
}

do_install() {
    # Overlays are conventionally placed alongside the main DTBs, in
    # /boot/overlays/, so the bootloader (U-Boot uEnv.txt) can apply them.
    install -d ${D}/boot/overlays
    install -m 0644 ${B}/hilglebone-uart-overlay.dtbo \
        ${D}/boot/overlays/hilglebone-uart-overlay.dtbo
}

FILES:${PN} = "/boot/overlays/hilglebone-uart-overlay.dtbo"

# Don't try to strip a binary blob.
INHIBIT_PACKAGE_STRIP = "1"
