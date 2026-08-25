#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ENV_NAME="heltec-v4"
ERASE_FIRST=false
FULLCLEAN=false
JUST_BUILD=false

show_usage() {
	echo "Usage: $0 [--heltec|-H] [--erase|-E] [--fullclean|-F] [--just-build|-B]"
	echo "  --heltec, -H     Use the Heltec V4 expansion environment ($ENV_NAME)"
	echo "  --erase, -E      Erase flash before upload"
	echo "  --fullclean, -F  Run PlatformIO fullclean first"
	echo "  --just-build, -B Compile only; do not upload or open the monitor"
}

format_duration() {
	local total_seconds="$1"
	local hours=$((total_seconds / 3600))
	local minutes=$(((total_seconds % 3600) / 60))
	local seconds=$((total_seconds % 60))

	if [ "$hours" -gt 0 ]; then
		printf "%dh %02dm %02ds" "$hours" "$minutes" "$seconds"
	else
		printf "%dm %02ds" "$minutes" "$seconds"
	fi
}

resolve_pio() {
	if command -v pio >/dev/null 2>&1; then
		command -v pio
	elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
		echo "$HOME/.platformio/penv/bin/pio"
	else
		echo "PlatformIO CLI not found. Install PlatformIO Core or the VS Code extension." >&2
		return 1
	fi
}

for arg in "$@"; do
	case "$arg" in
		--heltec|-H)
			;;
		--erase|-E)
			ERASE_FIRST=true
			;;
		--fullclean|-F)
			FULLCLEAN=true
			;;
		--just-build|-B)
			JUST_BUILD=true
			;;
		--help|-h)
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

if [ "$JUST_BUILD" = true ] && [ "$ERASE_FIRST" = true ]; then
	echo "--erase requires a connected device and cannot be combined with --just-build." >&2
	exit 1
fi

if ! grep -q "^\[env:${ENV_NAME}\]" platformio.ini; then
	echo "Environment '$ENV_NAME' not found in platformio.ini" >&2
	exit 1
fi

PIO="$(resolve_pio)"
BUILD_START_TS="$(date +%s)"

if [ "$FULLCLEAN" = true ]; then
	echo "[PIO] Full clean ($ENV_NAME)..."
	"$PIO" run -e "$ENV_NAME" -t fullclean
fi

if [ "$JUST_BUILD" = true ]; then
	echo "[PIO] Building $ENV_NAME..."
	"$PIO" run -e "$ENV_NAME"
else
	if [ "$ERASE_FIRST" = true ]; then
		echo "[PIO] Erasing device flash ($ENV_NAME)..."
		"$PIO" run -e "$ENV_NAME" -t erase
	fi
	echo "[PIO] Uploading $ENV_NAME..."
	"$PIO" run -e "$ENV_NAME" -t upload
fi

BUILD_END_TS="$(date +%s)"
echo "[PIO] Build completed in $(format_duration "$((BUILD_END_TS - BUILD_START_TS))")."

ELF_PATH=".pio/build/${ENV_NAME}/firmware.elf"
BIN_PATH=".pio/build/${ENV_NAME}/firmware.bin"
if [ -f "$ELF_PATH" ]; then
	ELF_SHA="$(shasum -a 256 "$ELF_PATH" | awk '{print $1}')"
	echo "[PIO] ELF SHA256: $ELF_SHA"
	echo "[PIO] Runtime monitor should show: ELF file SHA256: ${ELF_SHA:0:16}"
fi
if [ -f "$BIN_PATH" ]; then
	BIN_SHA="$(shasum -a 256 "$BIN_PATH" | awk '{print $1}')"
	echo "[PIO] BIN SHA256: $BIN_SHA"
fi

if [ "$JUST_BUILD" = false ]; then
	echo "[PIO] Opening monitor ($ENV_NAME)..."
	"$PIO" run -e "$ENV_NAME" -t monitor
fi