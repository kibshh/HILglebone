#!/usr/bin/env bash
#
# HILglebone Yocto environment bootstrap.
#
# SOURCE this file (do not execute it) from any shell where you want to do
# Yocto work. It sets TEMPLATECONF so that oe-init-build-env copies our
# templates into the newly created build directory, then sources
# oe-init-build-env itself.
#
# Usage:
#   source yocto-env.sh             # uses default build dir ../yocto-build
#   source yocto-env.sh my-build    # uses ../my-build instead
#
# Assumed layout (siblings under <workspace>):
#   <workspace>/poky
#   <workspace>/meta-arm
#   <workspace>/meta-ti
#   <workspace>/HILglebone    <-- this repo
#
# If your layout differs, edit POKY_DIR below.

# Refuse to run if not sourced. Executing this would set variables in a
# subshell that immediately disappears, which is worse than failing loudly.
if [ "${BASH_SOURCE[0]}" = "${0}" ]; then
    echo "yocto-env.sh: must be sourced, not executed." >&2
    echo "  run:  source yocto-env.sh" >&2
    exit 1
fi

# Locate the project root (directory containing this script).
_HILG_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Assumed workspace layout: poky is a sibling of HILglebone.
POKY_DIR="${_HILG_ROOT}/../poky"

# Default build directory; can be overridden with a positional arg.
BUILD_DIR="${1:-${_HILG_ROOT}/../yocto-build}"

# Point TEMPLATECONF at our template directory so that on first init,
# oe-init-build-env populates conf/local.conf and conf/bblayers.conf from
# our versioned samples. Since scarthgap, TEMPLATECONF must point inside a
# layer at <layer>/conf/templates/<template-name>, so we host the templates
# inside meta-hilglebone rather than as a free-standing directory.
export TEMPLATECONF="${_HILG_ROOT}/yocto/meta-hilglebone/conf/templates/default"

if [ ! -d "${POKY_DIR}" ]; then
    echo "yocto-env.sh: poky not found at ${POKY_DIR}" >&2
    echo "  clone it first:  git clone -b scarthgap git://git.yoctoproject.org/poky.git" >&2
    unset _HILG_ROOT POKY_DIR BUILD_DIR
    return 1
fi

if [ ! -f "${POKY_DIR}/oe-init-build-env" ]; then
    echo "yocto-env.sh: ${POKY_DIR}/oe-init-build-env not found" >&2
    unset _HILG_ROOT POKY_DIR BUILD_DIR
    return 1
fi

echo "yocto-env.sh: TEMPLATECONF=${TEMPLATECONF}"
echo "yocto-env.sh: BUILD_DIR=${BUILD_DIR}"

# oe-init-build-env changes the current directory to BUILD_DIR when it
# finishes. Sourcing it propagates that cd into our shell, which is the
# desired behavior.
cd "${POKY_DIR}" && source oe-init-build-env "${BUILD_DIR}"

# Clean up helper variables from the environment (keep TEMPLATECONF so
# subsequent re-inits still pick up the template if you ever delete the
# build dir and re-source this script).
unset _HILG_ROOT POKY_DIR BUILD_DIR
