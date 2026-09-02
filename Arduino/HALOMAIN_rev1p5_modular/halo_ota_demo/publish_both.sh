#!/usr/bin/env bash
set -euo pipefail

VERSION=""
CHANNEL="dev"
PROFILE="trepo-dev"
BUCKET="halo-ota-dev"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$SCRIPT_DIR"
LCD_FQBN="esp32:esp32:esp32s3:PartitionScheme=custom,FlashSize=8M,USBMode=hwcdc,CDCOnBoot=cdc,PSRAM=opi"
ARDUINO_CLI="${ARDUINO_CLI_BIN:-arduino-cli}"

usage() {
  echo "Usage: $0 --version X.Y.Z [--channel dev] [--profile trepo-dev] [--bucket halo-ota-dev]"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)
      VERSION="${2:-}"
      shift 2
      ;;
    --channel)
      CHANNEL="${2:-}"
      shift 2
      ;;
    --profile)
      PROFILE="${2:-}"
      shift 2
      ;;
    --bucket)
      BUCKET="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1"
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$VERSION" ]]; then
  echo "Error: --version is required."
  usage
  exit 1
fi

if [[ "${ARDUINO_CLI}" != "arduino-cli" && ! -x "${ARDUINO_CLI}" ]]; then
  echo "Error: ARDUINO_CLI_BIN is not executable: ${ARDUINO_CLI}"
  exit 1
fi

if [[ "${ARDUINO_CLI}" == "arduino-cli" ]] && ! command -v arduino-cli >/dev/null 2>&1; then
  echo "Error: arduino-cli not found on PATH"
  exit 1
fi

BUILD_PATH="/tmp/halo_lcd_build_${VERSION//./_}"

cd "$REPO_DIR"

echo "==> Using Arduino CLI: ${ARDUINO_CLI}"
"${ARDUINO_CLI}" version || true

echo "==> Publishing Sense $VERSION"
python3 tools/generate_version_header.py --version "$VERSION" --board sense
cp firmware/shared/Version.h firmware/halo_sense_prod/Version.h
echo "  Synced Version.h → halo_sense_prod/"
python3 tools/ota/publish_ota.py --channel "$CHANNEL" --profile "$PROFILE" --bucket "$BUCKET"

# Keep the binary's built-in OTA env in agreement with the channel we publish to.
#
# OTA_DEFAULT_ENV now defaults to "prod" so a plain `arduino-cli compile` is
# ship-safe (SHIP_CHECKLIST §1). Without this, `--channel dev` would build a
# PROD-pointing binary and publish it to the DEV channel — devices on dev would
# update once and then start looking at prod. Channel and env must match.
# Empty-array expansion under `set -u` is fatal on bash 3.2, which is what macOS
# ships -- and this array is empty in exactly one case: --channel prod. So the
# PROD publish always died here, right after the Sense had already been pushed:
#   ./publish_both.sh: line 94: OTA_ENV_FLAGS[@]: unbound variable
# The LCD half has therefore never completed for prod, which is why the prod
# channel had a Sense manifest and no LCD one. Expanded defensively below.
OTA_ENV_FLAGS=()
if [[ "$CHANNEL" != "prod" ]]; then
  OTA_ENV_FLAGS=(--build-property "compiler.cpp.extra_flags=-DOTA_DEFAULT_ENV=\"$CHANNEL\"")
  echo "  Building for OTA env '$CHANNEL' (non-prod)"
else
  echo "  Building for OTA env 'prod' (compiled-in default)"
fi

echo "==> Building and publishing LCD $VERSION"
python3 tools/generate_version_header.py --version "$VERSION" --board lcd
cp firmware/shared/Version.h firmware/halo_lcd_prod/Version.h
echo "  Synced Version.h → halo_lcd_prod/"
"${ARDUINO_CLI}" compile \
  --fqbn "$LCD_FQBN" \
  ${OTA_ENV_FLAGS[@]+"${OTA_ENV_FLAGS[@]}"} \
  --build-path "$BUILD_PATH" \
  firmware/halo_lcd_prod/halo_lcd_prod.ino

python3 tools/ota/publish_lcd_ota.py \
  --channel "$CHANNEL" \
  --version "$VERSION" \
  --bin "$BUILD_PATH/halo_lcd_prod.ino.bin" \
  --profile "$PROFILE"

echo "==> Completed publish for version $VERSION"
