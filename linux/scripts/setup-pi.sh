#!/bin/bash
#
# Set up mt32-pi (Linux build) on a fresh Raspberry Pi OS Lite (64-bit).
#
#   curl -fsSL https://raw.githubusercontent.com/j3tm0t0/mt32-pi/linux-port/linux/scripts/setup-pi.sh | sudo bash -s -- --hat
#   # or, from a checkout:
#   sudo linux/scripts/setup-pi.sh --hat
#
# What it does:
#   1. installs build dependencies and builds/installs /usr/local/bin/mt32-pi
#   2. creates the data directory /var/lib/mt32-pi (mt32-pi.cfg, roms/, soundfonts/)
#      and downloads the GeneralUser GS SoundFont
#   3. configures the boot files: GPIO UART for MIDI, I2C, I2S DAC overlay,
#      serial console removed
#   4. installs and enables the systemd service (user "mt32pi")
# A reboot is needed afterwards for the boot settings to take effect.
#
# Re-running is safe: settings are kept in marked blocks and existing
# mt32-pi.cfg files are not overwritten.
#

set -euo pipefail

REPO_URL=https://github.com/j3tm0t0/mt32-pi.git
REPO_BRANCH=linux-port
SRC_DIR=/usr/local/src/mt32-pi
DATA_DIR=/var/lib/mt32-pi
SERVICE_USER=mt32pi
DAC=hifiberry-dac
ALSA_DEVICE=
HAT=0
SOUNDFONT=1
REBOOT=ask
BOOT_DIR=${BOOT_DIR:-}

GUGS_URL=https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/main/GeneralUser-GS.sf2

usage()
{
	cat <<EOF
Usage: sudo $0 [options]

  --hat                 Configure the front panel of a typical mt32-pi HAT:
                        I2S DAC, simple_buttons (GPIO 17/27/22/23), SSD1306 OLED 128x64
  --dac NAME            I2S DAC overlay: hifiberry-dac (PCM5102A, default), hifiberry-dacplus
                        (PCM5122), iqaudio-dacplus, ... or "none" (HDMI/headphone/USB audio)
  --alsa-device DEV     ALSA device for [linux] alsa_device (default: derived from --dac)
  --no-soundfont        Do not download the GeneralUser GS SoundFont
  --repo URL            Git repository to build from (default: $REPO_URL)
  --branch NAME         Branch (default: $REPO_BRANCH)
  --reboot / --no-reboot
  -h, --help
EOF
}

while [ $# -gt 0 ]; do
	case $1 in
		--hat) HAT=1 ;;
		--dac) DAC=$2; shift ;;
		--alsa-device) ALSA_DEVICE=$2; shift ;;
		--no-soundfont) SOUNDFONT=0 ;;
		--repo) REPO_URL=$2; shift ;;
		--branch) REPO_BRANCH=$2; shift ;;
		--reboot) REBOOT=yes ;;
		--no-reboot) REBOOT=no ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage; exit 1 ;;
	esac
	shift
done

log() { printf '\n\033[1;32m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mWARNING: %s\033[0m\n' "$*" >&2; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root (sudo $0 ...)"
[ "$(dpkg --print-architecture)" = arm64 ] || warn "not arm64; this script targets Raspberry Pi OS 64-bit"

# Raspberry Pi model decides which UART is on GPIO 14/15
MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo unknown)
case $MODEL in
	*"Raspberry Pi 5"*|*"Compute Module 5"*) PI5=1 ;;
	*) PI5=0 ;;
esac
echo "Model: $MODEL"

if [ -z "$BOOT_DIR" ]; then
	if [ -f /boot/firmware/config.txt ]; then BOOT_DIR=/boot/firmware; else BOOT_DIR=/boot; fi
fi
CONFIG_TXT=$BOOT_DIR/config.txt
CMDLINE_TXT=$BOOT_DIR/cmdline.txt

#
# 1. Build
#
log "Installing build dependencies"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
	build-essential cmake git pkg-config ca-certificates curl \
	libasound2-dev libfluidsynth-dev alsa-utils i2c-tools

# Use the checkout this script lives in, if any; otherwise clone
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)
if [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/../CMakeLists.txt" ] && [ -d "$SCRIPT_DIR/../../src" ]; then
	SRC_DIR=$(cd "$SCRIPT_DIR/../.." && pwd)
	log "Using source tree $SRC_DIR"
else
	log "Fetching $REPO_URL ($REPO_BRANCH) into $SRC_DIR"
	if [ -d "$SRC_DIR/.git" ]; then
		git -C "$SRC_DIR" fetch --depth 1 origin "$REPO_BRANCH"
		git -C "$SRC_DIR" checkout -B "$REPO_BRANCH" FETCH_HEAD
	else
		git clone --depth 1 --branch "$REPO_BRANCH" "$REPO_URL" "$SRC_DIR"
	fi
fi

# Only the submodules the Linux build needs (circle-stdlib is large and not used)
git -C "$SRC_DIR" submodule update --init --depth 1 external/munt external/inih

log "Building mt32-pi (this takes a few minutes)"
cmake -S "$SRC_DIR/linux" -B "$SRC_DIR/build-linux" -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC_DIR/build-linux" -j"$(nproc)"
install -m 755 "$SRC_DIR/build-linux/mt32-pi" /usr/local/bin/mt32-pi
echo "Installed /usr/local/bin/mt32-pi"

#
# 2. Service user and data directory
#
log "Setting up user '$SERVICE_USER' and $DATA_DIR"
if ! id "$SERVICE_USER" >/dev/null 2>&1; then
	useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi
for GROUP in audio dialout gpio i2c; do
	getent group "$GROUP" >/dev/null && usermod -aG "$GROUP" "$SERVICE_USER"
done

mkdir -p "$DATA_DIR/roms" "$DATA_DIR/soundfonts"

# Let the login user copy ROMs/SoundFonts in without sudo
LOGIN_USER=${SUDO_USER:-}
if [ -n "$LOGIN_USER" ] && [ "$LOGIN_USER" != root ] && id "$LOGIN_USER" >/dev/null 2>&1; then
	usermod -aG "$SERVICE_USER" "$LOGIN_USER"
	echo "Added $LOGIN_USER to group $SERVICE_USER (log in again to write to $DATA_DIR)"
fi

if [ "$SOUNDFONT" = 1 ] && [ ! -f "$DATA_DIR/soundfonts/GeneralUser-GS.sf2" ]; then
	log "Downloading GeneralUser GS SoundFont"
	if curl -fL --retry 3 -o "$DATA_DIR/soundfonts/GeneralUser-GS.sf2.part" "$GUGS_URL"; then
		mv "$DATA_DIR/soundfonts/GeneralUser-GS.sf2.part" "$DATA_DIR/soundfonts/GeneralUser-GS.sf2"
		# mt32-pi picks up an FX profile with the same base name as the SoundFont
		cp "$SRC_DIR/sdcard/soundfonts/GeneralUser GS v1.511.cfg" "$DATA_DIR/soundfonts/GeneralUser-GS.cfg"
	else
		rm -f "$DATA_DIR/soundfonts/GeneralUser-GS.sf2.part"
		warn "SoundFont download failed; put .sf2 files into $DATA_DIR/soundfonts yourself"
	fi
fi

# ALSA device for the configured DAC
if [ -z "$ALSA_DEVICE" ]; then
	case $DAC in
		none) ALSA_DEVICE=default ;;
		hifiberry-*) ALSA_DEVICE="plughw:CARD=sndrpihifiberry,DEV=0" ;;
		iqaudio-*) ALSA_DEVICE="plughw:CARD=IQaudIODAC,DEV=0" ;;
		*) ALSA_DEVICE=default; warn "unknown DAC '$DAC'; set [linux] alsa_device after checking 'aplay -l'" ;;
	esac
fi

if [ "$PI5" = 1 ]; then SERIAL_DEVICE=/dev/ttyAMA0; else SERIAL_DEVICE=/dev/serial0; fi

CFG=$DATA_DIR/mt32-pi.cfg
if [ ! -f "$CFG" ]; then
	log "Creating $CFG"
	cp "$SRC_DIR/sdcard/mt32-pi.cfg" "$CFG"

	set_option() # section key value
	{
		sed -i "/^\[$1\]/,/^\[/ s|^$2 = .*|$2 = $3|" "$CFG"
	}

	if [ "$HAT" = 1 ]; then
		set_option audio output_device i2s
		set_option control scheme simple_buttons
		set_option lcd type ssd1306_i2c
		set_option lcd width 128
		set_option lcd height 64
	fi

	cat >> "$CFG" <<EOF

# ------------------------------------------------------------------------------
# Linux build only (ignored by the bare-metal kernel, which logs a parse warning)
# ------------------------------------------------------------------------------
[linux]
alsa_device = $ALSA_DEVICE
alsa_periods = 2
midi_autoconnect = true
serial_device = $SERIAL_DEVICE
i2c_device = /dev/i2c-1
gpio_chip = auto
realtime_priority = 70
audio_cpu = -1
lock_memory = true
EOF
else
	echo "Keeping existing $CFG"
fi

chown -R "$SERVICE_USER:$SERVICE_USER" "$DATA_DIR"
chmod -R g+rwX "$DATA_DIR"
find "$DATA_DIR" -type d -exec chmod g+s {} +

#
# 3. Boot configuration
#
log "Configuring $CONFIG_TXT"
[ -f "$CONFIG_TXT" ] || die "$CONFIG_TXT not found (set BOOT_DIR)"
cp -n "$CONFIG_TXT" "$CONFIG_TXT.mt32pi-orig" || true

BLOCK="# BEGIN mt32-pi
# Added by mt32-pi setup-pi.sh; remove this block to undo
[all]
enable_uart=1
dtparam=i2c_arm=on"
if [ "$PI5" = 1 ]; then
	BLOCK="$BLOCK
dtparam=uart0=on"
else
	BLOCK="$BLOCK
# Put the full PL011 UART (stable 31250 baud) on GPIO 14/15
dtoverlay=disable-bt"
fi
if [ "$DAC" != none ]; then
	BLOCK="$BLOCK
dtoverlay=$DAC"
fi
BLOCK="$BLOCK
# END mt32-pi"

sed -i '/^# BEGIN mt32-pi$/,/^# END mt32-pi$/d' "$CONFIG_TXT"
printf '\n%s\n' "$BLOCK" >> "$CONFIG_TXT"

log "Removing the serial console from $CMDLINE_TXT"
if [ -f "$CMDLINE_TXT" ]; then
	cp -n "$CMDLINE_TXT" "$CMDLINE_TXT.mt32pi-orig" || true
	sed -i -E 's/ ?console=(serial0|ttyAMA0|ttyS0|ttyAMA10),[0-9]+//g; s/^ +//; s/ +$//' "$CMDLINE_TXT"
	cat "$CMDLINE_TXT"
else
	warn "$CMDLINE_TXT not found"
fi

if command -v systemctl >/dev/null && [ -d /run/systemd/system ]; then
	# Bluetooth would otherwise try to use the UART we just took over (Pi 3/4)
	[ "$PI5" = 0 ] && systemctl disable --now hciuart.service 2>/dev/null || true
	for TTY in serial0 ttyAMA0 ttyS0; do
		systemctl disable --now "serial-getty@$TTY.service" 2>/dev/null || true
	done
fi

#
# 4. Service
#
log "Installing systemd service"
install -m 644 "$SRC_DIR/linux/mt32-pi.service" /etc/systemd/system/mt32-pi.service
if command -v systemctl >/dev/null && [ -d /run/systemd/system ]; then
	systemctl daemon-reload
	systemctl enable mt32-pi.service
else
	warn "systemd not running; enable mt32-pi.service manually"
fi

log "Done"
cat <<EOF
Data directory: $DATA_DIR
  roms/        put MT-32 / CM-32L ROM images here (not included)
  soundfonts/  $( [ -f "$DATA_DIR/soundfonts/GeneralUser-GS.sf2" ] && echo "GeneralUser GS installed" || echo "put .sf2 files here")
  mt32-pi.cfg  configuration ([linux] section at the end)

After the reboot:
  journalctl -u mt32-pi -f      # log
  aplay -l                      # check the DAC shows up
  aconnect -l                   # USB MIDI devices (connected automatically)
EOF

case $REBOOT in
	yes) reboot ;;
	no) echo "Reboot to apply the boot configuration." ;;
	*)
		if [ -t 0 ]; then
			read -r -p "Reboot now? [y/N] " ANSWER
			[ "${ANSWER:-n}" = y ] && reboot
		fi
		echo "Reboot to apply the boot configuration."
		;;
esac
