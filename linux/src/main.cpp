//
// main.cpp - mt32-pi for Linux entry point
//

#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <circle/logger.h>
#include <fatfs/ff.h>

#include "config.h"
#include "mt32pilinux.h"

LOGMODULE("main");

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

namespace
{
	CMT32PiLinux* g_pApp = nullptr;

	void SignalHandler(int)
	{
		if (g_pApp)
			g_pApp->Stop();
	}

	void Usage(const char* pProgram)
	{
		fprintf(stderr,
			"mt32-pi " MT32_PI_VERSION " (Linux)\n"
			"\n"
			"Usage: %s [options]\n"
			"\n"
			"  -d, --data-dir DIR     Directory containing mt32-pi.cfg, roms/ and soundfonts/ (default: .)\n"
			"  -c, --config FILE      Config file (default: <data-dir>/mt32-pi.cfg)\n"
			"  -u, --usb-dir DIR      Optional second volume searched for roms/ and soundfonts/ (like USB: on the Pi)\n"
			"  -s, --synth NAME       Force initial synth: mt32 | soundfont\n"
			"  -D, --device NAME      ALSA PCM device (overrides [linux] alsa_device)\n"
			"  -m, --midi-in ADDR     Also connect this ALSA sequencer source (e.g. 20:0 or a client name)\n"
			"  -S, --stats SECS       Log audio render/xrun statistics every SECS seconds\n"
			"  -p, --play FILE.mid    Live: also play a Standard MIDI File through the real-time path\n"
			"  -l, --loop             Live: loop the --play file\n"
			"  -r, --render IN.mid    Offline: render a Standard MIDI File instead of running live\n"
			"  -o, --output OUT.wav   Offline: output WAV file (default: out.wav)\n"
			"  -v, --verbose          Debug logging\n"
			"  -h, --help             Show this help\n",
			pProgram);
	}

	std::string Dirname(const std::string& Path)
	{
		const size_t nSlash = Path.find_last_of('/');
		if (nSlash == std::string::npos)
			return ".";
		if (nSlash == 0)
			return "/";
		return Path.substr(0, nSlash);
	}
}

int main(int argc, char** argv)
{
	std::string DataDir;
	std::string ConfigPath;
	std::string USBDir;
	std::string ALSADevice;
	std::string MIDISource;
	std::string RenderInput;
	std::string PlayFile;
	bool bLoop = false;
	std::string RenderOutput = "out.wav";
	TOptional<TSynth> ForcedSynth;
	unsigned nStatsInterval = 0;
	bool bVerbose = false;

	static const option Options[] = {
		{"data-dir", required_argument, nullptr, 'd'},
		{"config", required_argument, nullptr, 'c'},
		{"usb-dir", required_argument, nullptr, 'u'},
		{"synth", required_argument, nullptr, 's'},
		{"device", required_argument, nullptr, 'D'},
		{"midi-in", required_argument, nullptr, 'm'},
		{"stats", required_argument, nullptr, 'S'},
		{"play", required_argument, nullptr, 'p'},
		{"loop", no_argument, nullptr, 'l'},
		{"render", required_argument, nullptr, 'r'},
		{"output", required_argument, nullptr, 'o'},
		{"verbose", no_argument, nullptr, 'v'},
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0},
	};

	int nOption;
	while ((nOption = getopt_long(argc, argv, "d:c:u:s:D:m:S:p:lr:o:vh", Options, nullptr)) != -1)
	{
		switch (nOption)
		{
			case 'd': DataDir = optarg; break;
			case 'c': ConfigPath = optarg; break;
			case 'u': USBDir = optarg; break;
			case 'D': ALSADevice = optarg; break;
			case 'm': MIDISource = optarg; break;
			case 'S': nStatsInterval = static_cast<unsigned>(atoi(optarg)); break;
			case 'p': PlayFile = optarg; break;
			case 'l': bLoop = true; break;
			case 'r': RenderInput = optarg; break;
			case 'o': RenderOutput = optarg; break;
			case 'v': bVerbose = true; break;

			case 's':
				if (!strcasecmp(optarg, "mt32"))
					ForcedSynth = TSynth::MT32;
				else if (!strcasecmp(optarg, "soundfont"))
					ForcedSynth = TSynth::SoundFont;
				else
				{
					fprintf(stderr, "Unknown synth '%s'\n", optarg);
					return 1;
				}
				break;

			case 'h':
				Usage(argv[0]);
				return 0;

			default:
				Usage(argv[0]);
				return 1;
		}
	}

	if (DataDir.empty())
		DataDir = ConfigPath.empty() ? "." : Dirname(ConfigPath);

	// Map FatFs volumes onto host directories
	ff_set_volume_root("SD", DataDir.c_str());
	ff_set_volume_root("USB", USBDir.c_str());

	if (bVerbose)
		CLogger::Get()->SetLogLevel(LogDebug);

	LOGNOTE("mt32-pi " MT32_PI_VERSION " (Linux), data dir: %s", DataDir.c_str());

	CConfig Config;
	if (ConfigPath.empty())
	{
		if (!Config.Initialize("SD:mt32-pi.cfg"))
			LOGWARN("Unable to find or parse config file; using defaults");
	}
	else
	{
		// Explicit config path is a host path; read it through a temporary volume
		ff_set_volume_root("CFG", Dirname(ConfigPath).c_str());
		const std::string Name = ConfigPath.substr(ConfigPath.find_last_of('/') == std::string::npos ? 0 : ConfigPath.find_last_of('/') + 1);
		if (!Config.Initialize(("CFG:" + Name).c_str()))
			LOGWARN("Unable to find or parse config file '%s'; using defaults", ConfigPath.c_str());
	}

	if (Config.SystemVerbose)
		CLogger::Get()->SetLogLevel(LogDebug);

	if (!ALSADevice.empty())
		Config.LinuxALSADevice = ALSADevice.c_str();

	CMT32PiLinux App(Config);
	g_pApp = &App;

	struct sigaction Action{};
	Action.sa_handler = SignalHandler;
	sigemptyset(&Action.sa_mask);
	sigaction(SIGINT, &Action, nullptr);
	sigaction(SIGTERM, &Action, nullptr);

	if (!App.Initialize(ForcedSynth))
		return 1;

	const int nResult = RenderInput.empty()
		? App.Run(MIDISource.c_str(), nStatsInterval, PlayFile.c_str(), bLoop)
		: App.RenderFile(RenderInput.c_str(), RenderOutput.c_str(), 2.0);

	g_pApp = nullptr;
	return nResult;
}
