#!/usr/bin/env bash
# Encrypt the asset pack and publish it as release assets for CI (CLAUDE.md section 10).
#   tools/convert/publish_pack.sh <pack dir> <release tag> <age identity file>
# Deterministic tar (sorted, mtime 0, numeric owner 0) -> gzip -n -> age (X25519 recipient of the identity) ->
# 1900 MB chunks (release assets max 2 GB). Writes tools/ci/assetpack.txt (tag + chunk sha256s; CI's cache key
# and checksum list) and uploads the chunks with gh (replacing assets of the same name).
# The identity (private key) stays in work/keys/ and is the ASSETPACK_KEY secret:
#   gh secret set ASSETPACK_KEY < work/keys/assetpack.agekey
# Never upload or commit anything decrypted.
set -euo pipefail
PACK="$(cd "$1" && pwd)"
TAG="$2"
KEY="$3"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[[ -f "$PACK/manifest.txt" ]] || { echo "no manifest.txt in $PACK"; exit 1; }
RECIPIENT="$(age-keygen -y "$KEY")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/rlios_pub_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu \
  -C "$(dirname "$PACK")" -cf - "$(basename "$PACK")" | gzip -n -9 |
  age -r "$RECIPIENT" -o "$WORK/assetpack.tar.gz.age"
split -b 1900m -d -a 2 "$WORK/assetpack.tar.gz.age" "$WORK/assetpack.tar.gz.age.part"
rm "$WORK/assetpack.tar.gz.age"

OUT="$REPO_ROOT/tools/ci/assetpack.txt"
{
  echo "# Encrypted asset pack for CI (tools/convert/publish_pack.sh). Chunks are release assets of the tag."
  echo "tag $TAG"
  echo "pack_manifest_sha256 $(sha256sum "$PACK/manifest.txt" | cut -d' ' -f1)"
  (cd "$WORK" && sha256sum assetpack.tar.gz.age.part* | awk '{print "chunk " $1 " " $2}')
} > "$OUT"
cat "$OUT"

if ! gh release view "$TAG" >/dev/null 2>&1; then
  gh release create "$TAG" --title "$TAG (encrypted asset pack for CI)" --prerelease \
    --notes "age-encrypted asset pack chunks for CI. Not usable without the repository secret."
fi
gh release upload "$TAG" "$WORK"/assetpack.tar.gz.age.part* --clobber
echo "uploaded to release $TAG"
