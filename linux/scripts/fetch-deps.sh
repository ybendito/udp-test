#!/usr/bin/env bash
# Vendored Boost into deps/ at a pinned commit (see README.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEPS="${ROOT}/deps"
BOOST_COMMIT="8c3ca159ca9e5ac4b56ced6a6f146d5fef3650bc"

mkdir -p "${DEPS}"

ensure_repo() {
    local name="$1"
    local url="$2"
    local commit="$3"
    local path="${DEPS}/${name}"

    if [[ ! -d "${path}/.git" ]]; then
        echo "Cloning ${name} ..."
        git clone "${url}" "${path}"
    else
        echo "${name} already present at ${path}"
    fi

    (
        cd "${path}"
        git fetch --depth 1 origin "${commit}"
        git checkout --detach "${commit}"
    )
}

ensure_repo boost "https://github.com/boostorg/boost.git" "${BOOST_COMMIT}"

echo "Initializing Boost submodules required by boostudp ..."
(
    cd "${DEPS}/boost"
    git submodule update --init \
        libs/align \
        libs/asio \
        libs/assert \
        libs/config \
        libs/core \
        libs/integer \
        libs/io \
        libs/mpl \
        libs/optional \
        libs/predef \
        libs/preprocessor \
        libs/smart_ptr \
        libs/static_assert \
        libs/system \
        libs/throw_exception \
        libs/type_traits \
        libs/utility \
        libs/winapi
)

echo "Done."
echo "  Boost: ${DEPS}/boost @ ${BOOST_COMMIT}"
