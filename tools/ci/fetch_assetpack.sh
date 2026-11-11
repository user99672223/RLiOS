#!/usr/bin/env bash
# CI: fetch (or reuse cached) encrypted asset pack chunks, verify checksums, decrypt into <dest>.
#   ASSETPACK_KEY=<age identity> tools/ci/fetch_assetpack.sh <chunk cache dir> <dest dir>
# Prints the decrypted pack path (<dest>/assetpack). Never prints or uploads decrypted content.
set -euo pipefail
CACHE="$1"
DEST="$2"
LIST="$(dirname "$0")/assetpack.txt"
TAG="$(awk '$1=="tag"{print $2}' "$LIST")"
mkdir -p "$CACHE" "$DEST"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
ok=1
while read -r kind hash name; do
  [[ "$kind" == chunk ]] || continue
  if [[ ! -f "$CACHE/$name" || "$(sha "$CACHE/$name")" != "$hash" ]]; then ok=0; fi
done < "$LIST"
if [[ $ok == 0 ]]; then
  echo "downloading asset pack chunks from release $TAG"
  rm -f "$CACHE"/assetpack.tar.gz.age.part*
  gh release download "$TAG" --pattern 'assetpack.tar.gz.age.part*' --dir "$CACHE"
else
  echo "asset pack chunks restored from cache"
fi
while read -r kind hash name; do
  [[ "$kind" == chunk ]] || continue
  got="$(sha "$CACHE/$name")"
  [[ "$got" == "$hash" ]] || { echo "checksum mismatch for $name"; exit 1; }
done < "$LIST"
KEYFILE="$(mktemp)"
trap 'rm -f "$KEYFILE"' EXIT
chmod 600 "$KEYFILE"
printf '%s\n' "$ASSETPACK_KEY" > "$KEYFILE"
cat $(awk '$1=="chunk"{print $3}' "$LIST" | sort | sed "s|^|$CACHE/|") | age -d -i "$KEYFILE" | tar -xzf - -C "$DEST"
[[ -f "$DEST/assetpack/manifest.txt" ]] || { echo "decrypted pack has no manifest"; exit 1; }
echo "asset pack decrypted: $(du -sh "$DEST/assetpack" | cut -f1), $(grep -c '^file' "$DEST/assetpack/manifest.txt") files"
