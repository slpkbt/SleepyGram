#!/bin/bash
set -e

configure_flags=()
if [[ "${SLEEPYGRAM_CI_CCACHE:-}" == 1 ]]; then
    unset CCACHE_DISABLE
    export CCACHE_COMPILERCHECK=content
    export CCACHE_CONFIGPATH=/dev/null
    export CCACHE_TEMPDIR=/tmp/ccache
    test -w "$CCACHE_DIR"
    ccache --version
    ccache --zero-stats
    trap 'ccache --show-stats && du -sh "$CCACHE_DIR"' EXIT
    configure_flags+=(-D SLEEPYGRAM_CI_CCACHE=ON)
fi

cd Telegram
./configure.sh "$@" "${configure_flags[@]}"
build_flags=()
if [[ "${KEEP_GOING:-}" == 1 ]]; then
    build_flags+=(-- -k 0)
fi
cmake --build ../out --config "${CONFIG:-Release}" "${build_flags[@]}"
