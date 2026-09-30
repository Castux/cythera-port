#!/bin/sh
# Build a minimal static FreeType (TrueType driver + monochrome rasterizer)
# into third_party/freetype/, where the Makefile picks it up for hinted text.
# Only needed where no system FreeType is available (pkg-config freetype2).
# Usage: tools/build_freetype.sh path/to/freetype-2.x.tar.xz (or unpacked source dir)
# Environment: CC (default cc; may include flags, e.g. "clang -arch x86_64"), AR (default ar).
set -e
SRC="$1"
[ -n "$SRC" ] || { echo "usage: $0 freetype-2.x.tar.xz|freetype-2.x/"; exit 1; }
OUT="$(cd "$(dirname "$0")/.." && pwd)/third_party/freetype"
CC="${CC:-cc}"
AR="${AR:-ar}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -f "$SRC" ]; then
  tar -xf "$SRC" -C "$TMP"
  SRC="$(echo "$TMP"/freetype-*)"
fi
[ -f "$SRC/include/ft2build.h" ] || { echo "$SRC: not a FreeType source tree"; exit 1; }

# only the modules needed to rasterise TrueType fonts in monochrome
mkdir -p "$TMP/cfg" "$TMP/obj"
cat > "$TMP/cfg/ftmodule.h" <<'EOF'
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
EOF

for f in base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbitmap base/ftmm \
         truetype/truetype sfnt/sfnt psnames/psnames raster/raster gzip/ftgzip; do
  echo "  CC $f.c"
  $CC -O2 -DFT2_BUILD_LIBRARY '-DFT_CONFIG_MODULES_H=<ftmodule.h>' \
    -I"$TMP/cfg" -I"$SRC/include" -c "$SRC/src/$f.c" -o "$TMP/obj/$(basename $f).o"
done

rm -rf "$OUT"
mkdir -p "$OUT/lib"
cp -R "$SRC/include" "$OUT/include"
"$AR" rcs "$OUT/lib/libfreetype.a" "$TMP"/obj/*.o
echo "FreeType built in $OUT"
