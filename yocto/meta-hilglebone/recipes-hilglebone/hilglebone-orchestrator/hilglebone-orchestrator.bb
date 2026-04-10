SUMMARY = "HILglebone Python orchestrator"
DESCRIPTION = "Reads scenario YAMLs, drives the STM32 emulator over UART, \
collects responses from the DUT, and runs as a systemd service."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The orchestrator's source lives outside the layer (in <repo>/bbb/) so that
# native development on the host doesn't need to round-trip through Yocto.
# Extend FILESEXTRAPATHS up to the repo root so SRC_URI can pick it up via
# file://.
#
# THISDIR = .../yocto/meta-hilglebone/recipes-hilglebone/hilglebone-orchestrator
# Going up 4 levels lands us at the HILglebone repo root, which contains bbb/.
FILESEXTRAPATHS:prepend := "${THISDIR}/../../../../:"

SRC_URI = " \
    file://bbb \
    file://hilglebone.service \
"

S = "${WORKDIR}"

# We're shipping pure-Python sources plus a systemd unit -- no compilation.
# Skip the configure/compile tasks BitBake would otherwise expect.
do_configure[noexec] = "1"
do_compile[noexec] = "1"

inherit systemd

SYSTEMD_SERVICE:${PN} = "hilglebone.service"
SYSTEMD_AUTO_ENABLE = "enable"

# Where on the target the orchestrator gets installed. /opt is the conventional
# location for "third-party / vendor" software that does not belong to the
# distro.
HILGLEBONE_INSTALL_DIR = "/opt/hilglebone"

do_install() {
    # Install the Python package tree.
    install -d ${D}${HILGLEBONE_INSTALL_DIR}
    cp -r ${WORKDIR}/bbb ${D}${HILGLEBONE_INSTALL_DIR}/

    # Install the systemd unit.
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/hilglebone.service \
        ${D}${systemd_unitdir}/system/hilglebone.service
}

FILES:${PN} = " \
    ${HILGLEBONE_INSTALL_DIR} \
    ${systemd_unitdir}/system/hilglebone.service \
"

# Runtime deps. RDEPENDS pulls these into the image automatically when this
# recipe is included, so users don't have to repeat them in IMAGE_INSTALL.
#
# We use the full "python3" metapackage rather than "python3-core" so that
# stdlib modules like json, asyncio, logging.handlers, etc. are available
# without having to enumerate every python3-* subpackage. Slim this down
# to specific submodules later if image size becomes a concern.
RDEPENDS:${PN} = "python3 python3-pyserial python3-pyyaml"
