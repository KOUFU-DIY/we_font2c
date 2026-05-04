#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_DIR="$ROOT/builder/src"
INC_DIR="$ROOT/builder/include"
OUT="$ROOT/font2c"

CC=${CC:-cc}
PKG_CONFIG=${PKG_CONFIG:-pkg-config}

CFLAGS=$("$PKG_CONFIG" --cflags freetype2)
LIBS=$("$PKG_CONFIG" --libs freetype2)

exec "$CC" -std=c11 -O2 -Wall -Wextra -pedantic -I"$INC_DIR" $CFLAGS \
    "$SRC_DIR/common.c" "$SRC_DIR/json_config.c" "$SRC_DIR/font_build.c" "$SRC_DIR/main.c" \
    -o "$OUT" $LIBS
