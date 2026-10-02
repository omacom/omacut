#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH="" cd -- "$(dirname -- "$0")/.." && pwd)"
STAGE="$(mktemp -d "$ROOT/build/release-test.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM

ARCH="$(uname -m)"
NAME="omacut-test-linux-$ARCH"
"$ROOT/bin/release-artifact" test "$STAGE/native"
(cd "$STAGE/native" && sha256sum -c "$NAME.tar.gz.sha256")
tar -xzf "$STAGE/native/$NAME.tar.gz" -C "$STAGE"
cmp "$ROOT/build/omacut" "$STAGE/$NAME/omacut"
cmp "$ROOT/pkgbuild/omacut.desktop" "$STAGE/$NAME/omacut.desktop"
cmp "$ROOT/pkgbuild/omacut.svg" "$STAGE/$NAME/omacut.svg"
cmp "$ROOT/LICENSE" "$STAGE/$NAME/LICENSE"
cmp "$ROOT/README.md" "$STAGE/$NAME/README.md"
test -x "$STAGE/$NAME/omacut"
grep -Fx "Version: test" "$STAGE/$NAME/BUILDINFO"
grep -Fx "Architecture: $ARCH" "$STAGE/$NAME/BUILDINFO"
grep -Fx "Commit: $(git -C "$ROOT" rev-parse HEAD)" "$STAGE/$NAME/BUILDINFO"
grep -Fx "Qt: $(qmake6 -query QT_VERSION)" "$STAGE/$NAME/BUILDINFO"

if "$ROOT/bin/release-artifact" 'invalid/version' "$STAGE/invalid" > "$STAGE/invalid.log" 2>&1; then
  echo "The packager accepted an invalid version." >&2
  exit 1
fi
grep -q 'Usage:' "$STAGE/invalid.log"
test ! -e "$STAGE/invalid"

if GIT_DIR="$STAGE/missing.git" "$ROOT/bin/release-artifact" test "$STAGE/metadata" > "$STAGE/metadata.log" 2>&1; then
  echo "The packager accepted missing commit metadata." >&2
  exit 1
fi
test ! -e "$STAGE/metadata"

# Isolate a copy of the actual binary and change its ELF e_machine field to the
# other supported architecture. The original build and source stay untouched.
mkdir -p "$STAGE/fixture/bin" "$STAGE/fixture/build" "$STAGE/fixture/pkgbuild"
cp "$ROOT/bin/release-artifact" "$STAGE/fixture/bin/"
cp "$ROOT/build/omacut" "$STAGE/fixture/build/"
cp "$ROOT/pkgbuild/omacut.desktop" "$ROOT/pkgbuild/omacut.svg" "$STAGE/fixture/pkgbuild/"
cp "$ROOT/LICENSE" "$ROOT/README.md" "$STAGE/fixture/"
case "$ARCH" in
  aarch64) printf '\076\000' ;;
  x86_64) printf '\267\000' ;;
  *) echo "Unsupported test architecture: $ARCH" >&2; exit 1 ;;
esac | dd of="$STAGE/fixture/build/omacut" bs=1 seek=18 conv=notrunc 2>/dev/null
if "$STAGE/fixture/bin/release-artifact" test "$STAGE/mismatch" > "$STAGE/mismatch.log" 2>&1; then
  echo "The packager accepted a binary for the wrong architecture." >&2
  exit 1
fi
grep -q 'does not match release architecture' "$STAGE/mismatch.log"
test ! -e "$STAGE/mismatch"

echo "Release artifact checks passed."
