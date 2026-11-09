# mt32-pi for Linux (userspace)

An experimental build of mt32-pi that runs as a normal Linux program, e.g. on
Raspberry Pi OS Lite on a Pi 4/5, instead of as a bare-metal kernel.

It reuses mt32-pi's own synth code (MT-32 emulation via Munt, SoundFonts via
FluidSynth), MIDI parser, config file, ROM/SoundFont scanning and custom SysEx
commands. The Circle-specific parts are replaced:

| Bare metal (Circle)                     | Linux                                                   |
|-----------------------------------------|---------------------------------------------------------|
| PWM / HDMI / I²S audio drivers          | ALSA PCM (any card: I²S DAC HAT, HDMI, USB audio...)    |
| USB MIDI host stack, GPIO UART, Pisound | ALSA sequencer (USB MIDI, rtpmidid, ttymidi, Pisound driver...) |
| Core 2 busy-polling audio task          | Audio thread, `SCHED_FIFO`, blocking ALSA writes        |
| Core 0 main loop                        | Control thread, `poll()` on the sequencer               |
| FatFs on SD/USB                         | Plain directories (`-d DATA_DIR`, optional `-u USB_DIR`) |
| Patched FluidSynth + zone allocator     | Stock distro `libfluidsynth`                            |

Not ported (yet): LCD/OLED displays, buttons/encoder, MiSTer control, and
the built-in networking (use `rtpmidid` for RTP-MIDI and `scp`/Samba for
files instead).

## Building

On the Pi (Raspberry Pi OS Lite, 64-bit) or in the provided Docker image:

```sh
sudo apt install build-essential cmake git pkg-config libasound2-dev libfluidsynth-dev
git submodule update --init external/munt external/inih
cmake -S linux -B build-linux
cmake --build build-linux -j4
sudo install -m 755 build-linux/mt32-pi /usr/local/bin/
```

On an Apple Silicon Mac, the Docker image is native arm64 and matches
Raspberry Pi OS (Debian bookworm):

```sh
docker build -t mt32pi-linux -f linux/Dockerfile linux
docker run --rm -v "$PWD":/src -w /src mt32pi-linux \
    sh -c 'cmake -S linux -B build-linux && cmake --build build-linux -j'
```

Add `-DMT32PI_ASAN=ON` for an AddressSanitizer/UBSan build.

## Data directory

Same layout as the SD card:

```
DATA_DIR/
├── mt32-pi.cfg     (copy from sdcard/mt32-pi.cfg)
├── roms/           MT-32 / CM-32L control + PCM ROMs
└── soundfonts/     .sf2 files (+ optional .cfg FX profiles)
```

`[audio] sample_rate` and `chunk_size` (ALSA period size) apply as before;
`output_device` is ignored. Linux-only settings go into an extra section:

```ini
[linux]
alsa_device = default        # ALSA PCM, e.g. hw:0, plughw:CARD=sndrpihifiberry
alsa_periods = 2             # buffer = chunk_size * alsa_periods
midi_autoconnect = true      # subscribe to all MIDI sources, incl. hot-plugged USB
realtime_priority = 70       # SCHED_FIFO priority of the audio thread, 0 = off
audio_cpu = -1               # pin the audio thread to a CPU core, -1 = no pinning
lock_memory = true           # mlockall() if RLIMIT_MEMLOCK is unlimited
```

(The bare-metal build logs a harmless parse warning for this unknown section,
so keep it in the Linux copy of the config only.)

## Running

```sh
mt32-pi -d ~/mt32-pi                      # live: ALSA MIDI in -> ALSA out
mt32-pi -d ~/mt32-pi -D hw:0 -S 10        # pick device, log stats every 10 s
mt32-pi -d ~/mt32-pi -p song.mid          # play a MIDI file through the live path
mt32-pi -d ~/mt32-pi -r song.mid -o out.wav   # offline render to WAV
```

The program shows up as ALSA sequencer client `mt32-pi`; with
`midi_autoconnect` every hardware MIDI input is connected automatically.
Otherwise use `aconnect` or `-m CLIENT:PORT`.

The custom SysEx commands work as on the Pi (`F0 7D 01 xx F7` ROM set,
`F0 7D 02 xx F7` SoundFont, `F0 7D 03 xx F7` synth, `F0 7D 04 xx F7` MT-32
stereo swap). "Reboot" (`F0 7D 00 F7`) exits the process; the systemd unit
restarts it.

### As a service

```sh
sudo install -m 644 linux/mt32-pi.service /etc/systemd/system/
sudo mkdir -p /var/lib/mt32-pi && sudo cp -r mt32-pi.cfg roms soundfonts /var/lib/mt32-pi/
sudo systemctl daemon-reload && sudo systemctl enable --now mt32-pi
journalctl -u mt32-pi -f
```

The unit grants `LimitRTPRIO`/`LimitMEMLOCK`, so the audio thread runs
`SCHED_FIFO` with locked memory. When run by hand without those limits, it
falls back to normal scheduling and says so in the log.

## Measuring real-time behaviour

`-S SECS` logs, per interval, the number of audio periods, average/maximum
render time against the period budget, and ALSA underruns (xruns).
`linux/scripts/loadtest.sh` plays a dense ~200-voice SoundFont workload with
and without `stress-ng` loading all cores:

```sh
sudo apt install stress-ng
linux/scripts/loadtest.sh -b build-linux/mt32-pi -d ~/mt32-pi -D hw:0 -t 60
linux/scripts/loadtest.sh -b build-linux/mt32-pi -d ~/mt32-pi -D hw:0 -t 60 -y mt32
```

Output latency is `chunk_size * alsa_periods / sample_rate`
(256 × 2 / 48000 ≈ 10.7 ms by default; try `chunk_size = 128`).
