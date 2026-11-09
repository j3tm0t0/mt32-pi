# Build/test environment matching Raspberry Pi OS Lite (64-bit, Debian bookworm).
# On an Apple Silicon Mac this runs natively as linux/arm64.
#
#   docker build -t mt32pi-linux -f linux/Dockerfile linux
#   docker run --rm -v "$PWD":/src -w /src mt32pi-linux \
#       sh -c 'cmake -S linux -B build-linux && cmake --build build-linux -j'

FROM debian:bookworm

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		build-essential cmake git pkg-config \
		libasound2-dev libfluidsynth-dev \
		alsa-utils fluid-soundfont-gm stress-ng gdb \
	&& rm -rf /var/lib/apt/lists/*
