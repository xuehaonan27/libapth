#!/bin/bash
set -e

find ./build/lib -type f \( -name "*.so" -o -name "*.so.*" \) | while read f; do
  if file "$f" | grep -q "ELF"; then
    OLD_RPATH=$(patchelf --print-rpath "$f" 2>/dev/null || echo "")
    NEW_RPATH="$CUSTOM_SYSROOT/lib/x86_64-linux-gnu:$CUSTOM_PREFIX/lib64:$CUSTOM_PREFIX/lib"
    if [ -n "$OLD_RPATH" ]; then
      NEW_RPATH="$NEW_RPATH:$OLD_RPATH"
    fi
    patchelf --set-interpreter "$CUSTOM_DYNLINKER" "$f" 2>/dev/null || true
    patchelf --set-rpath "$NEW_RPATH" "$f" 2>/dev/null || true
    echo "  Patched: $f"
  fi
done
