#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENV_NAME="heltec-v4"
PROJECT_NAME="camillia-monitor"
ASSUME_YES=false
NO_CLEAN=false

show_usage() {
	echo "Usage: $0 [-y|--yes] [--no-clean]"
	echo "  Builds $ENV_NAME, packages factory and OTA images, commits the"
	echo "  release state, tags v<version>, and publishes a GitHub release."
	echo ""
	echo "  -y, --yes   Bump the patch version without prompting"
	echo "  --no-clean  Reuse current .pio output instead of running fullclean"
}

resolve_pio() {
	if command -v pio >/dev/null 2>&1; then
		command -v pio
	elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
		echo "$HOME/.platformio/penv/bin/pio"
	else
		echo "PlatformIO CLI not found." >&2
		return 1
	fi
}

resolve_esptool() {
	if python3 -m esptool version >/dev/null 2>&1; then
		ESPTOOL=(python3 -m esptool)
	elif python -m esptool version >/dev/null 2>&1; then
		ESPTOOL=(python -m esptool)
	elif command -v esptool.py >/dev/null 2>&1; then
		ESPTOOL=(esptool.py)
	else
		echo "esptool not found. Install it with: pip install esptool" >&2
		return 1
	fi
}

remote_tag_exists() {
	local tag="$1"
	git ls-remote --exit-code --tags origin "refs/tags/${tag}" >/dev/null 2>&1
}

for arg in "$@"; do
	case "$arg" in
		-y|--yes)
			ASSUME_YES=true
			;;
		--no-clean)
			NO_CLEAN=true
			;;
		-h|--help)
			show_usage
			exit 0
			;;
		*)
			echo "Unknown argument: $arg" >&2
			show_usage >&2
			exit 1
			;;
	esac
done

if ! grep -q "^\[env:${ENV_NAME}\]" platformio.ini; then
	echo "Environment '$ENV_NAME' not found in platformio.ini" >&2
	exit 1
fi
if ! command -v gh >/dev/null 2>&1; then
	echo "GitHub CLI (gh) is required. Install it and run: gh auth login" >&2
	exit 1
fi

PIO="$(resolve_pio)"
CURRENT="$(tr -d '\n' < VERSION 2>/dev/null || true)"
PREV_TAG="$(git describe --tags --abbrev=0 2>/dev/null || true)"
echo "Current version: ${CURRENT:-unknown}"
echo "Latest git tag:  ${PREV_TAG:-none}"

if [ "$ASSUME_YES" = true ]; then
	VERSION_SOURCE="$PREV_TAG"
	if [ -z "$VERSION_SOURCE" ]; then
		VERSION_SOURCE="$CURRENT"
	fi
	if [[ "$VERSION_SOURCE" =~ ^v?([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
		VERSION="${BASH_REMATCH[1]}.${BASH_REMATCH[2]}.$((BASH_REMATCH[3] + 1))"
		echo "Auto-bumped ${VERSION_SOURCE} -> v${VERSION}"
	else
		echo "Cannot auto-bump '$VERSION_SOURCE'; expected vMAJOR.MINOR.PATCH." >&2
		exit 1
	fi
else
	read -r -p "New version (for example 1.0.0): " VERSION
	VERSION="${VERSION#v}"
fi

if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	echo "Invalid version '$VERSION'; expected MAJOR.MINOR.PATCH." >&2
	exit 1
fi
TAG="v${VERSION}"

if git rev-parse "$TAG" >/dev/null 2>&1 || remote_tag_exists "$TAG"; then
	echo "Tag $TAG already exists. Choose a new version." >&2
	exit 1
fi

ORIGINAL_VERSION="$CURRENT"
RELEASE_COMMITTED=false
on_exit() {
	local exit_code=$?
	if [ "$exit_code" -ne 0 ] && [ "$RELEASE_COMMITTED" = false ]; then
		printf '%s\n' "$ORIGINAL_VERSION" > VERSION
		echo "Release failed; restored VERSION and created no release commit." >&2
	fi
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "$TAG" > VERSION
echo "Updated VERSION to $TAG"

if [ "$NO_CLEAN" = false ]; then
	echo "Running full clean for $ENV_NAME..."
	"$PIO" run -e "$ENV_NAME" -t fullclean
else
	echo "Skipping full clean (--no-clean)."
fi

echo "Building $ENV_NAME..."
"$PIO" run -e "$ENV_NAME"

BUILD_DIR=".pio/build/${ENV_NAME}"
for artifact in bootloader.bin partitions.bin firmware.bin firmware.elf; do
	if [ ! -f "${BUILD_DIR}/${artifact}" ]; then
		echo "Missing build artifact: ${BUILD_DIR}/${artifact}" >&2
		exit 1
	fi
done

BOOT_APP0="$(find "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions" \
	-name boot_app0.bin 2>/dev/null | head -1)"
if [ -z "$BOOT_APP0" ]; then
	echo "boot_app0.bin not found in the installed Arduino ESP32 framework." >&2
	exit 1
fi
resolve_esptool

rm -rf dist
mkdir -p dist
FACTORY_IMAGE="dist/${PROJECT_NAME}-heltec-${TAG}.bin"
OTA_IMAGE="dist/${PROJECT_NAME}-heltec-${TAG}-ota.bin"
ELF_IMAGE="dist/${PROJECT_NAME}-heltec-${TAG}.elf"

echo "Packaging factory image..."
"${ESPTOOL[@]}" --chip esp32s3 merge_bin \
	-o "$FACTORY_IMAGE" \
	-fm dio \
	-ff 80m \
	-fs 16MB \
	0x0 "${BUILD_DIR}/bootloader.bin" \
	0x8000 "${BUILD_DIR}/partitions.bin" \
	0xe000 "$BOOT_APP0" \
	0x10000 "${BUILD_DIR}/firmware.bin"
cp "${BUILD_DIR}/firmware.bin" "$OTA_IMAGE"
cp "${BUILD_DIR}/firmware.elf" "$ELF_IMAGE"
(
	cd dist
	shasum -a 256 ./*.bin > SHA256SUMS
)

git add -A
git commit -m "Release $TAG"
RELEASE_COMMITTED=true
git push
git tag "$TAG"
git push origin "$TAG"

NOTES_ARGS=(--generate-notes)
if [ -s RELEASE_NOTES.md ]; then
	NOTES_ARGS=(--notes-file RELEASE_NOTES.md --generate-notes)
fi

echo "Publishing GitHub release $TAG..."
gh release create "$TAG" \
	--title "$TAG" \
	"${NOTES_ARGS[@]}" \
	"$FACTORY_IMAGE" \
	"$OTA_IMAGE" \
	dist/SHA256SUMS

echo "Release $TAG published."
gh release view "$TAG" --json url -q .url