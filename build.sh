#!/usr/bin/env bash
set -o pipefail

# Cross-compile MiSTer with Arm GNU Toolchain 10.2-2020.11.
# Override ARM_TOOLCHAIN_DIR when the toolchain is installed elsewhere.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TOOLCHAIN_DIR="${ARM_TOOLCHAIN_DIR:-$HOME/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf}"
export PATH="$TOOLCHAIN_DIR/bin:$PATH"

cd "$SCRIPT_DIR" || exit 1
command -v arm-none-linux-gnueabihf-gcc >/dev/null || {
	echo "ARM toolchain not found: $TOOLCHAIN_DIR" >&2
	exit 1
}

make "$@" 2>&1 | tee /tmp/mister_fbui_build.log
exit "${PIPESTATUS[0]}"
