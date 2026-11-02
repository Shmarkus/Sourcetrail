#!/usr/bin/env bash
# Sets up the build toolchain for this fork and installs the Conan dependencies.
#
#   scripts/fork/bootstrap.sh          install what is missing, then print the export lines
#   scripts/fork/bootstrap.sh --env    only print the export lines
#
# Typical use:
#   scripts/fork/bootstrap.sh
#   eval "$(scripts/fork/bootstrap.sh --env)"
#   cmake --preset fork_release
#
# Everything goes into $ST_TOOLCHAIN (default: $HOME/.cache/sourcetrail-toolchain):
#   venv/        Python venv with conan and aqtinstall
#   qt/          Qt from aqt
#   conan_home/  CONAN_HOME, so ~/.conan2 is never touched
# The script is idempotent: steps whose result already exists are skipped.
set -euo pipefail

CONAN_VERSION="2.34.0"
AQT_VERSION="3.3.0"
QT_VERSION="6.8.2"
QT_ARCH="linux_gcc_64"
QT_MODULES=(qt5compat qtimageformats)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TOOLCHAIN="${ST_TOOLCHAIN:-$HOME/.cache/sourcetrail-toolchain}"
VENV="$TOOLCHAIN/venv"
QT_ROOT="$TOOLCHAIN/qt"
QT_PREFIX="$QT_ROOT/$QT_VERSION/gcc_64"
CONAN_HOME_DIR="$TOOLCHAIN/conan_home"

print_env() {
  echo "export CONAN_HOME=\"$CONAN_HOME_DIR\""
  echo "export CMAKE_PREFIX_PATH=\"$QT_PREFIX\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}\""
  echo "export PATH=\"$VENV/bin:$QT_PREFIX/bin:\$PATH\""
}

log() {
  echo "bootstrap: $*" >&2
}

case "${1:-}" in
  --env)
    print_env
    exit 0
    ;;
  "") ;;
  *)
    echo "usage: $0 [--env]" >&2
    exit 2
    ;;
esac

mkdir -p "$TOOLCHAIN"

if [[ ! -x "$VENV/bin/conan" || ! -x "$VENV/bin/aqt" ]]; then
  log "creating venv in $VENV"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet "conan==$CONAN_VERSION" "aqtinstall==$AQT_VERSION" >&2
else
  log "venv present"
fi

if [[ ! -x "$QT_PREFIX/bin/qmake" || ! -d "$QT_PREFIX/lib/cmake/Qt6Core5Compat" ]]; then
  log "installing Qt $QT_VERSION ($QT_ARCH) into $QT_ROOT"
  # aqt writes aqtinstall.log into the working directory.
  (cd "$TOOLCHAIN" && "$VENV/bin/aqt" install-qt linux desktop "$QT_VERSION" "$QT_ARCH" -O "$QT_ROOT" -m "${QT_MODULES[@]}" >&2)
else
  log "Qt $QT_VERSION present"
fi

export CONAN_HOME="$CONAN_HOME_DIR"
mkdir -p "$CONAN_HOME"

log "installing Conan dependencies (locked by conan.lock)"
(cd "$ROOT_DIR" &&
  "$VENV/bin/conan" install . -s build_type=Release -of .conan/gcc/ -b missing -pr:a .conan/gcc/profile \
    --lockfile conan.lock >&2)

print_env
