#!/bin/bash
# Builds the release files into firmware/: the update image, the first-install image, their SHA-256
# hashes and the signed manifest. Needs ESP-IDF 5.2.2 loaded and the private signing key:
#   POLIBOT_SIGNING_KEY=/path/to/key.pem tools/release.sh "What changed, in one line"
set -euo pipefail
cd "$(dirname "$0")/.."
NOTES="${1:?usage: tools/release.sh \"release notes\"}"
KEY="${POLIBOT_SIGNING_KEY:?set POLIBOT_SIGNING_KEY to the private key file}"
PUB=polibot_native/main/update_key.pem
VERSION="$(cat polibot_native/version.txt)"
openssl pkey -in "$KEY" -pubout | cmp -s - "$PUB" || { echo "The key does not belong to $PUB" >&2; exit 1; }

B="$(mktemp -d)"
trap 'rm -rf "$B"' EXIT
idf.py -C polibot_native -B "$B" -DSDKCONFIG="$B/sdkconfig" -DIDF_TARGET=esp32s3 build
(cd "$B" && python -m esptool --chip esp32s3 merge_bin -o full.bin @flash_args)

UPDATE="polibot-$VERSION-update.bin"
FULL="polibot-$VERSION-full.bin"
cp "$B/polibot_native.bin" "firmware/$UPDATE"
cp "$B/full.bin" "firmware/$FULL"
if grep -aqF "$HOME" "firmware/$UPDATE" "firmware/$FULL"; then echo "A local folder path is inside the image" >&2; exit 1; fi
(cd firmware && shasum -a 256 polibot-*.bin > SHA256SUMS)

python3 - "$VERSION" "firmware/$UPDATE" "$NOTES" > firmware/latest.json <<'PY'
import hashlib, json, os, sys
version, path, notes = sys.argv[1:]
data = open(path, 'rb').read()
print(json.dumps({'version': version, 'board': 'esp32s3-n16r8', 'file': os.path.basename(path),
                  'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'notes': notes}, indent=1))
PY
[ "$(wc -c < firmware/latest.json)" -le 2048 ] || { echo "latest.json is over the 2048 bytes devices accept: shorten the notes" >&2; exit 1; }
openssl dgst -sha256 -sign "$KEY" -out firmware/latest.json.sig firmware/latest.json
openssl dgst -sha256 -verify "$PUB" -signature firmware/latest.json.sig firmware/latest.json
cat firmware/latest.json
