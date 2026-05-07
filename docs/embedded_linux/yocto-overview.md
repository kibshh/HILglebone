# Yocto: what it is and how it builds an image

A general primer on the Yocto Project. For *our specific* image and
customizations, see [linux-image.md](linux-image.md).

## What Yocto actually is

Yocto is **not a Linux distribution**. It's a build system that
generates a custom distribution. You feed it recipes (".bb files"), it
fetches sources, cross-compiles them, and packages everything into a
bootable image (kernel + bootloader + rootfs) tailored for a specific
piece of hardware.

The closest mental model: imagine Gentoo's portage tree, but designed
from the start for embedded targets and cross-compilation. Every
component — kernel, bootloader, libc, busybox, your application — is a
recipe that BitBake (the executor) builds from source against a
target-specific toolchain.

The reference distribution Yocto ships with is called **Poky**. When
you `git clone yoctoproject.org/poky`, you get BitBake, the core meta
layer (`meta/`), and a sample distro config. Most projects start from
Poky and add layers on top.

## Key concepts

### BitBake

The build executor. Reads recipes, computes dependency graphs, runs
tasks in parallel. Roughly: `bitbake <recipe-or-image-name>`. It's the
"make" of the Yocto world, but operating on recipes instead of
Makefiles.

### Recipe (`.bb` file)

A recipe describes how to build one piece of software. Variables like:

- `SUMMARY`, `LICENSE` — metadata
- `SRC_URI` — where to fetch sources (`git://`, `file://`, `https://`)
- `DEPENDS` — build-time dependencies on other recipes
- `RDEPENDS` — runtime dependencies (pulled into the final image)
- `do_configure`, `do_compile`, `do_install` — shell-task overrides

A recipe produces one or more **packages** (.deb, .rpm, or .ipk
depending on `PACKAGE_CLASSES`).

### Bbappend (`.bbappend` file)

A way to extend an existing recipe without forking it. Lives in
*your* layer; appends/overrides variables in someone else's recipe.
Used to tweak upstream packages without copy-pasting the whole `.bb`.

We use this in
[u-boot-bb.org_%.bbappend](../../yocto/meta-hilglebone/recipes-bsp/u-boot/u-boot-bb.org_%.bbappend)
to inject `FDTOVERLAYS` into the bb.org U-Boot's generated
`extlinux.conf`.

### Layer

A directory containing related recipes. Layers stack: each adds /
overrides things on top of the layers it depends on. Conventional
naming: `meta-<something>`.

Examples:
- `meta/` — Poky's core layer
- `meta-openembedded/meta-oe` — common open-source recipes
- `meta-ti` / `meta-beagle` — TI BSP and BeagleBoard-specific recipes
- `meta-hilglebone/` — **our** customisations

A layer is just a directory with a `conf/layer.conf` file declaring its
name, priority, recipe glob patterns, and dependencies.

### Machine

A target hardware definition. `MACHINE = "beaglebone"` in
`build/conf/local.conf` selects everything BBB-specific: SoC family,
kernel defconfig, U-Boot variant, default console, etc. Machine
definitions live in `meta-<bsp>/conf/machine/<name>.conf`.

### Distro

A higher-level configuration affecting policy: package format, init
system, libc, what's in `IMAGE_FEATURES`. `DISTRO = "poky"` is the
default. You rarely write your own distro for a project this small.

### Image

A "meta-recipe" describing what goes into the final rootfs. Defined by
a `.bb` that inherits `core-image` and sets `IMAGE_INSTALL`. BitBake
expands the dependency graph from the listed packages, builds them
all, then assembles a filesystem.

The output is in `tmp/deploy/images/<machine>/`:

- `<image>.wic` / `.wic.bz2` — disk image you flash to SD/eMMC
- `<image>.tar.bz2` — bare rootfs tarball
- `zImage` — kernel
- `*.dtb` — device tree blobs
- `MLO`, `u-boot.img` — bootloader stages

## Build process per recipe

For each recipe, BitBake runs an ordered chain of tasks:

```
do_fetch       -> grab sources from SRC_URI
do_unpack      -> extract into ${WORKDIR}
do_patch       -> apply any .patch files
do_configure   -> usually ./configure or cmake
do_compile     -> make / cmake --build
do_install     -> install into ${D} (sysroot)
do_package     -> split into runtime packages
do_rootfs      -> (image-only) assemble final filesystem
```

Each task has a *checksum* of its inputs; if nothing changed, it gets
skipped on the next build. This is how Yocto stays fast despite the
"build everything from source" model.

### sstate-cache

Build artefacts get cached under `sstate-cache/`. Sharing this cache
between builds (or developers) avoids rebuilding identical components
from scratch. CI systems often pre-populate it.

## Project layout convention

```
yocto/                          # vendored / submoduled layers (NOT product code)
├── poky/                       # upstream Poky checkout
├── meta-openembedded/          # community recipes
├── meta-ti/, meta-arm/, ...    # SoC vendor BSPs
└── meta-hilglebone/            # our layer (in this repo)

build/                          # generated; not tracked by VCS
├── conf/
│   ├── local.conf              # MACHINE=, DISTRO=, IMAGE_INSTALL extras
│   └── bblayers.conf           # which layers BitBake should load
├── tmp/                        # work area + outputs
│   ├── work/<arch>/<recipe>/   # per-recipe build dirs
│   └── deploy/images/<machine>/  # final outputs
└── sstate-cache/               # per-task artefact cache
```

The `build/` directory is created by sourcing Poky's environment
script: `source poky/oe-init-build-env build`.

## Variables you'll touch most often

| Variable | In | Purpose |
|----------|----|---------|
| `MACHINE` | `local.conf` | Target hardware |
| `DISTRO` | `local.conf` | Distro flavour |
| `IMAGE_INSTALL` | image recipe | Packages in final rootfs |
| `IMAGE_FEATURES` | image recipe | High-level extras (`debug-tweaks`, `ssh-server-openssh`) |
| `SRC_URI` | recipe | Where sources come from |
| `DEPENDS` / `RDEPENDS` | recipe | Build-time / runtime deps |
| `FILES:${PN}` | recipe | Which installed files belong to the package |
| `S` | recipe | Where source ends up after unpack |
| `D` | recipe | Staging dir for `do_install` |
| `WORKDIR` | recipe | Per-recipe scratch space |
| `BBPATH` | layer.conf | Where BitBake looks for files |

Append-style operators are critical:

- `:append` — pure string concatenation (you supply the leading space)
- `:prepend` — same, on the front
- `+=` / `=+` — adds with auto-spacing (avoid mixing with `:append`)
- `?=` — soft default (set if not already set)
- `??=` — weakest default

## Typical workflow

```bash
# One-time: source the build environment.
source yocto/poky/oe-init-build-env build

# Edit local.conf to set MACHINE, IMAGE_INSTALL extras, etc.
$EDITOR conf/local.conf

# Add layers to bblayers.conf.
bitbake-layers add-layer ../yocto/meta-hilglebone

# Build the image.
bitbake hilglebone-image

# Flash the result.
bmaptool copy tmp/deploy/images/beaglebone/hilglebone-image-*.wic.bz2 /dev/sdX
```

A clean build of a small image takes ~30–90 minutes the first time.
Incremental rebuilds (after editing one recipe) take seconds to a few
minutes.

## When something doesn't end up in the image

The fastest debugging:

```bash
# Where did the package go?
oe-pkgdata-util find-path /usr/bin/<binary>

# What's actually inside the rootfs?
ls tmp/work/beaglebone-poky-linux-gnueabi/<image-name>/*/rootfs/

# Did the recipe even build?
bitbake -e <recipe> | grep ^IMAGE_INSTALL=
```

For our specific image, see
[linux-image.md](linux-image.md).
