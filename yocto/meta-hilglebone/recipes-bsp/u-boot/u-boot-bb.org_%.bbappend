# HILglebone customizations for U-Boot's extlinux.conf
#
# The upstream uboot-extlinux-config.bbclass generates extlinux.conf but has
# no FDTOVERLAYS support. We fix two things:
#
# 1. Inject an FDTOVERLAYS line so U-Boot applies our UART1 overlay at boot.
# 2. Override UBOOT_EXTLINUX_CONSOLE to use the modern ttyS0 driver name
#    instead of the deprecated ttyO0 that comes from U-Boot's default env.

# Fix the console name: U-Boot's default env has console=ttyO0 (old omap-serial
# driver). Modern kernels use the 8250_omap driver and the node is ttyS0.
# Setting this explicitly avoids relying on U-Boot variable expansion.
UBOOT_EXTLINUX_CONSOLE = "console=ttyS0,115200n8"

# Use an explicit FDT path instead of FDTDIR. U-Boot's extlinux parser only
# processes the FDTOVERLAYS directive when FDT is set -- FDTDIR skips it.
UBOOT_EXTLINUX_FDT = "../am335x-boneblack.dtb"

# Post-process the generated extlinux.conf to add FDTOVERLAYS.
# This runs after do_create_extlinux_config (which writes ${B}/extlinux.conf)
# and before do_install / do_deploy (which copy it to DEPLOY_DIR_IMAGE).
do_add_fdtoverlays() {
    if [ -f ${B}/extlinux.conf ]; then
        if grep -q "FDTOVERLAYS" ${B}/extlinux.conf; then
            # FDTOVERLAYS line exists -- append our overlay if not already listed
            if ! grep -q "hilglebone-uart-overlay.dtbo" ${B}/extlinux.conf; then
                sed -i 's|^\([[:space:]]*FDTOVERLAYS .*\)|\1 /hilglebone-uart-overlay.dtbo|' \
                    ${B}/extlinux.conf
            fi
        else
            # No FDTOVERLAYS line -- insert one after FDT or FDTDIR
            sed -i '/^[[:space:]]*FDT\(DIR\)\? /a\\tFDTOVERLAYS /hilglebone-uart-overlay.dtbo' \
                ${B}/extlinux.conf
        fi
    fi
}
addtask add_fdtoverlays after do_create_extlinux_config before do_install
