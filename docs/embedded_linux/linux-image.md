# HILglebone Linux image: what we ship on the BBB

This describes the image we build for the BeagleBone Black: which
packages it includes, what's customised, where files live, and how the
boot process pulls everything together. For Yocto fundamentals see
[yocto-overview.md](yocto-overview.md).

The whole image is defined by one layer: `meta-hilglebone/`.

## Layer at a glance

```
yocto/meta-hilglebone/
├── conf/
│   └── layer.conf                          # priority 10, depends on core/oe/python
├── recipes-core/
│   └── images/
│       └── hilglebone-image.bb             # the image recipe (top-level)
├── recipes-hilglebone/
│   └── hilglebone-orchestrator/
│       ├── hilglebone-orchestrator.bb      # Python app + systemd unit
│       └── hilglebone.service              # systemd service file
└── recipes-bsp/
    ├── device-tree/
    │   ├── hilglebone-uart-overlay.bb      # compiles UART1 .dtbo
    │   └── files/hilglebone-uart-overlay.dts
    ├── usb-gadget-net/
    │   ├── usb-gadget-net.bb               # g_ether + systemd-networkd
    │   └── files/{g_ether.conf, g_ether.options, 80-usb0.network}
    └── u-boot/
        └── u-boot-bb.org_%.bbappend        # FDTOVERLAYS injection
```

Layer config: priority 10 (above Poky's 5), compatible with
`kirkstone` and `scarthgap` Yocto releases.

## Image recipe (`hilglebone-image.bb`)

The top of the recipe inherits `core-image` (a minimal bootable system
based on BusyBox) and adds packages on top.

### Always-installed packages

```
hilglebone-orchestrator    # our Python app + systemd unit
hilglebone-uart-overlay    # UART1 device-tree overlay (.dtbo)
kernel-modules             # convenience: pulls in standard tty drivers etc.
```

The orchestrator's runtime deps (`python3`, `python3-pyserial`,
`python3-pyyaml`) are NOT listed here — they're declared as `RDEPENDS`
in the orchestrator recipe, and BitBake resolves them automatically.

### Dev-only extras (gated by `HILGLEBONE_DEV=1`)

Set `HILGLEBONE_DEV = "1"` in `local.conf` to flip these on:

```
openssh-sftp-server   # file transfer (used by VS Code Remote, scp, etc.)
htop                  # process inspection
rsync                 # incremental code sync from host
usb-gadget-net        # the g_ether-based usb0 link (see below)
```

Plus the image features:

```
debug-tweaks           # blank root password, simple login
ssh-server-openssh     # full OpenSSH (not just sftp-server)
tools-debug            # gdb, strace, ltrace
```

The gating is implemented as a Python expression inside `IMAGE_INSTALL`:

```
IMAGE_INSTALL:append = "${@' openssh-sftp-server htop rsync usb-gadget-net' \
                          if d.getVar('HILGLEBONE_DEV') == '1' else ''}"
```

This keeps dev conveniences scoped to *this* image — running
`bitbake core-image-minimal` from the same build directory won't pick
them up.

### Boot files

```
IMAGE_BOOT_FILES:append = " hilglebone-uart-overlay.dtbo"
```

Tells `wic` to copy the UART overlay onto the FAT boot partition, so
U-Boot can find it via `FDTOVERLAYS` in `extlinux.conf`.

### Rootfs sizing

`IMAGE_ROOTFS_EXTRA_SPACE = "524288"` — 512 MiB headroom on top of the
strict-minimum size. BBB eMMC is 4 GiB and SD cards are 8 GiB+, so
this is comfortable.

## Component recipes

### `hilglebone-orchestrator`

The Python application that runs on the BBB, talks to the STM32 over
UART, and (later) routes to NATS.

- Sources: pulls from `<repo>/bbb/` via `file://bbb` in `SRC_URI`.
  `FILESEXTRAPATHS:prepend` walks up four directories from the recipe
  to reach the repo root, so the orchestrator code can stay outside
  the layer (developers can hack on it without round-tripping through
  Yocto).
- Install location: `/opt/hilglebone/bbb/`. `/opt` is the conventional
  spot for vendor / third-party software that doesn't belong to the
  distro.
- Inherits `systemd`. Installs `hilglebone.service` to
  `${systemd_unitdir}/system/`. `SYSTEMD_AUTO_ENABLE = "enable"` means
  the unit is enabled at first boot.
- `do_configure` and `do_compile` are no-ops — pure Python, no build
  step.
- Runtime deps: `python3 python3-pyserial python3-pyyaml`. The
  full `python3` metapackage is used (rather than `python3-core`) to
  avoid having to enumerate every stdlib submodule the orchestrator
  uses.

### `hilglebone-uart-overlay`

Compiles `hilglebone-uart-overlay.dts` into a `.dtbo` and deploys it
two places:

- `/boot/overlays/hilglebone-uart-overlay.dtbo` (rootfs)
- `${DEPLOYDIR}/hilglebone-uart-overlay.dtbo` (so `wic` can put it on
  the FAT partition via `IMAGE_BOOT_FILES`)

What the overlay does:

- Configures pinmux at `0x184`/`0x180` to UART1 mode 0
  (P9.24 = UART1_TXD, P9.26 = UART1_RXD)
- Enables the UART1 controller and binds it to the new pinmux

The pin offsets and mode bytes come from the AM335x TRM control module
section. The `8250_omap` driver picks up the enabled UART1 and exposes
it as `/dev/ttyS1`.

`COMPATIBLE_MACHINE = "beaglebone"` — BitBake silently skips this
recipe if you're building for anything else.

`DEPENDS = "dtc-native"` — pulls in a host-side device-tree compiler
to run during `do_compile`.

### `usb-gadget-net`

Configures USB Ethernet gadget (`g_ether`) so the BBB exposes a virtual
Ethernet interface (`usb0`) over the same micro-USB cable that
provides power. Three small files installed:

| File | Path on BBB | Purpose |
|------|-------------|---------|
| `g_ether.conf` | `/etc/modules-load.d/g_ether.conf` | tells systemd-modules-load to modprobe g_ether at boot |
| `g_ether.options` | `/etc/modprobe.d/g_ether.conf` | pins the gadget MACs (host = `02:bb:be:e0:01:01`, dev = `02:bb:be:e0:01:02`) |
| `80-usb0.network` | `/etc/systemd/network/80-usb0.network` | systemd-networkd config: `usb0` = `192.168.7.2/24`, no gateway |

Why pinned MACs? Otherwise g_ether generates random MACs each boot,
which means:

- The host-side predictable interface name (`enx<mac>`) changes every
  reboot — host scripts can't deterministically find the BBB.
- The host's `known_hosts`, ARP cache, and NetworkManager profiles all
  see a "new" device on each boot.

Pinned MACs use locally-administered addresses (first byte `02`).

Runtime deps: `kernel-module-g-ether` (g_ether must be a module, not
built-in), `systemd` (the configs are systemd-specific).

See also: [docs/bugs/host-mac-randomization.md](../bugs/host-mac-randomization.md)
for one of the gotchas around this on the host side.

### `u-boot-bb.org_%.bbappend`

Patches the bb.org U-Boot's generated `extlinux.conf`. Two fixes:

1. **Console name**: U-Boot's default env hardcodes `console=ttyO0`
   (the deprecated omap-serial driver). Modern kernels use
   `8250_omap`, where the same UART is `ttyS0`. We override with
   `UBOOT_EXTLINUX_CONSOLE = "console=ttyS0,115200n8"`.
2. **FDTOVERLAYS**: the upstream `uboot-extlinux-config.bbclass`
   doesn't write `FDTOVERLAYS`. Our `do_add_fdtoverlays` post-step
   `sed`s our overlay path into the generated config, so U-Boot
   applies the UART1 overlay before booting the kernel. We also force
   `UBOOT_EXTLINUX_FDT = "../am335x-boneblack.dtb"` (explicit FDT
   path) — `FDTOVERLAYS` is ignored by U-Boot's parser when `FDTDIR`
   is used instead.

The resulting `extlinux.conf` on the FAT partition looks like:

```
LABEL Poky (Yocto Project Reference Distro)
    KERNEL ../zImage
    FDT ../am335x-boneblack.dtb
    FDTOVERLAYS /hilglebone-uart-overlay.dtbo
    APPEND root=PARTUUID=${uuid} rootwait rw earlycon console=ttyS0,115200n8
```

## Boot flow (cold reset → orchestrator running)

```
ROM bootloader on AM335x
    │  reads first sectors of /dev/mmcblk0
    ▼
SPL (MLO) on FAT partition
    │  initialises DDR, loads u-boot.img
    ▼
U-Boot main stage
    │  runs `bootcmd = run findfdt; run init_console; run finduuid; run distro_bootcmd`
    │  scans MMC partitions for /extlinux/extlinux.conf
    ▼
extlinux.conf parsed
    │  loads zImage, am335x-boneblack.dtb, applies hilglebone-uart-overlay.dtbo
    ▼
Linux kernel boots
    │  applies merged DTB, brings up serial, mounts ext4 rootfs
    ▼
systemd init
    │  systemd-modules-load.service modprobes g_ether (pinned MACs apply)
    │  systemd-networkd brings usb0 up at 192.168.7.2/24
    │  hilglebone.service starts the orchestrator from /opt/hilglebone/bbb/
    ▼
Orchestrator running, ready to accept commands
```

## Filesystem layout (on the BBB)

```
/                                 # rootfs, /dev/mmcblk0p2 (ext4)
├── boot/
│   ├── overlays/
│   │   └── hilglebone-uart-overlay.dtbo    # rootfs copy (Yocto convention)
│   ├── zImage, *-ti                         # kernel images
│   └── dtb/                                 # all am335x DTBs
├── etc/
│   ├── modprobe.d/g_ether.conf              # pinned MACs
│   ├── modules-load.d/g_ether.conf          # autoload at boot
│   └── systemd/network/80-usb0.network      # static IP for usb0
├── opt/
│   └── hilglebone/
│       └── bbb/                             # Python orchestrator package tree
└── lib/systemd/system/
    └── hilglebone.service                   # systemd unit

/dev/mmcblk0p1                    # FAT boot partition (NOT mounted by default)
├── MLO                           # SPL
├── u-boot.img                    # U-Boot main
├── zImage                        # kernel (separate copy from rootfs)
├── am335x-*.dtb                  # all DTBs
├── hilglebone-uart-overlay.dtbo  # the overlay U-Boot reads
└── extlinux/
    └── extlinux.conf             # U-Boot boot config
```

The boot partition isn't auto-mounted because the kernel doesn't need
it after boot. Mount manually if you need to inspect / modify:
`mount /dev/mmcblk0p1 /mnt`.

## Networking (developer image only)

Two interfaces of interest:

| Interface | IP | Purpose |
|-----------|-----|--------|
| `usb0` | `192.168.7.2/24` | USB-Ethernet gadget to host laptop |
| `eth0` | DHCP (if RJ45 plugged) | Optional internet uplink |

Host-side, the BBB's gadget is named `enx02bbbee00101` (predictable,
derived from the pinned MAC). The repo includes a helper script
`bbb-usb-connect.sh` that finds the interface by MAC, assigns
`192.168.7.1/24` to the host side, and prints the SSH command.

## Drivers worth knowing about

| Driver | What it provides | Notes |
|--------|------------------|-------|
| `8250_omap` | UART0/1/...as `/dev/ttyS*` | Modern replacement for `omap-serial` |
| `g_ether` | USB Ethernet gadget | Loaded as a module so modprobe options apply |
| `omap_serial` | (Deprecated) | Would expose `/dev/ttyO*` instead of `ttyS*` — not used |
| Pinctrl-single | Pinmux at runtime | Driven by the device-tree pinmux nodes; the UART overlay relies on it |

## Where to make changes

| You want to... | Edit |
|----------------|------|
| Add a package to all images | `IMAGE_INSTALL:append` in `hilglebone-image.bb` |
| Add a dev-only package | The HILGLEBONE_DEV-gated line in the same file |
| Change the orchestrator code | `bbb/...` (the recipe pulls in via `file://bbb`) |
| Change the UART pinmux | `recipes-bsp/device-tree/files/hilglebone-uart-overlay.dts` |
| Change pinned MACs | `recipes-bsp/usb-gadget-net/files/g_ether.options` (and update `bbb-usb-connect.sh`) |
| Tweak boot args | `UBOOT_EXTLINUX_*` in the U-Boot bbappend, or `APPEND` line via the bbclass |

## See also

- [yocto-overview.md](yocto-overview.md) — how Yocto works in general
- [beagle_app/stm-link.md](beagle_app/stm-link.md) — orchestrator's serial bridge
- [docs/protocol/](../protocol/) — wire protocol the orchestrator speaks
- [docs/bugs/](../bugs/) — gotchas we've already paid for
