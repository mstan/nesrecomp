/*
 * mesen_shot.cpp - headless Mesen frame capture, a third reference for
 * runner/cyc alongside the TriCNES oracle and NES_MiSTer's APU.
 *
 *   mesen_shot <rom.nes> <out-dir> [--input FILE] [--frame-offset K]
 *              [--hw-options LIST] <frame> [frame ...]
 *
 * Runs the ROM on Mesen's core with no window, and writes <out-dir>/<frame>.png
 * at the end of each requested frame, so a recompiled build's --screenshot
 * output can be compared against an independent emulator's.
 *
 * --input takes the same button schedule as the host's --input (cyc_host.c):
 * `<frame> [buttons]` lines, buttons joined by +, `2:` for controller 2. A
 * game that needs input to reach the screen worth comparing - a level rather
 * than a title screen - can then be driven to the same place in both.
 *
 * --frame-offset K: host frame N is Mesen frame N+K. Frame numbers otherwise
 * line up (a demo with no input matches frame for frame), but a program's boot
 * can take a frame longer in Mesen than in NESRecomp and the TriCNES oracle;
 * SMB3 does, at every CPU/PPU alignment. Find K by comparing a frame from
 * before the first button press against Mesen N and N+1, then pass it so the
 * schedule reaches the program on the same frame of its own, and compare host
 * frame N with Mesen frame N+K.
 *
 * Mesen's test conventions (RecordedRomTest::UpdateSettings) are used: RAM
 * zeroed at power-on, no run-ahead, no frame skipping, sprite limit on, no
 * overclocking. NESRecomp's own machine powers RAM up with the $F0/$0F pattern
 * of the reference console instead, so a program that reads uninitialized RAM
 * may legitimately diverge.
 *
 * Build: tools/cyc/mesen_shot/build.ps1 <MesenCE checkout> <out-dir>
 */
#include "pch.h"

#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/SettingTypes.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/Shared/Video/VideoDecoder.h"
#include "Core/Shared/Interfaces/IConsole.h"
#include "Core/Shared/Interfaces/IInputProvider.h"
#include "Core/Shared/BaseControlDevice.h"
#include "Core/NES/Input/NesController.h"
#include "Core/NES/NesDefaultVideoFilter.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/PNGHelper.h"
#include "Utilities/VirtualFile.h"

#include <chrono>
#include <fstream>
#include <set>
#include <string>
#include <vector>

/* Mesen's 2C02 palette (NesDefaultVideoFilter.cpp). NesConfig::UserPalette is
 * filled by the UI, so a headless build has to supply the base colors itself
 * before GenerateFullColorPalette derives the emphasis rows. */
static constexpr uint32_t kPalette2C02[64] = {
	0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
	0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
	0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
	0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
	0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
	0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
	0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
	0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000
};

/* ---- the host's --input schedule ---- */

/* A NES pad as cyc_host.c writes it: A B Select Start Up Down Left Right, MSB
 * first. Mesen numbers its buttons differently, so the bits are translated
 * where they are applied. */
struct InputStep
{
	uint32_t Frame;
	uint8_t Port;
	uint8_t Buttons;
};

static bool NameIs(const char* s, size_t n, const char* name)
{
	size_t i = 0;
	for(; i < n && name[i]; i++) {
		char a = (s[i] >= 'a' && s[i] <= 'z') ? (char)(s[i] - 32) : s[i];
		if(a != name[i]) {
			return false;
		}
	}
	return i == n && !name[i];
}

static uint8_t ParseButtons(const char* s)
{
	static const struct { const char* Name; uint8_t Bit; } names[] = {
		{ "A", 0x80 },  { "B", 0x40 },    { "SELECT", 0x20 }, { "START", 0x10 },
		{ "UP", 0x08 }, { "DOWN", 0x04 }, { "LEFT", 0x02 },   { "RIGHT", 0x01 },
	};
	uint8_t b = 0;
	while(*s) {
		while(*s == '+' || *s == ' ' || *s == '\t') {
			s++;
		}
		size_t n = 0;
		while(s[n] && s[n] != '+' && s[n] != ' ' && s[n] != '\t' && s[n] != '\n' && s[n] != '\r') {
			n++;
		}
		if(!n) {
			break;
		}
		for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			if(NameIs(s, n, names[i].Name)) {
				b |= names[i].Bit;
				break;
			}
		}
		s += n;
	}
	return b;
}

/* Applies the schedule to whichever controller Mesen asks about, once per
 * frame. Mesen polls its input providers at the start of each frame, so the
 * step that is current for frame N is the one the host holds during frame N. */
class ScheduledInput : public IInputProvider
{
public:
	std::vector<InputStep> Steps;
	std::atomic<uint32_t>* Frame = nullptr;
	uint32_t FrameOffset = 0;   /* --frame-offset: Mesen frame N+K is host frame N */
	uint8_t Held[2] = {};
	size_t Next = 0;

	bool Load(const std::string& path)
	{
		std::ifstream f(path);
		if(!f) {
			std::cerr << "cannot read " << path << "\n";
			return false;
		}
		std::string line;
		while(std::getline(f, line)) {
			const char* p = line.c_str();
			while(*p == ' ' || *p == '\t') {
				p++;
			}
			if(*p == '#' || !*p || *p == '\r') {
				continue;
			}
			char* end;
			unsigned long frame = strtoul(p, &end, 10);
			if(end == p) {
				continue;
			}
			p = end;
			while(*p == ' ' || *p == '\t') {
				p++;
			}
			uint8_t port = 0;
			if(p[0] == '2' && p[1] == ':') {
				port = 1;
				p += 2;
			} else if(p[0] == '1' && p[1] == ':') {
				p += 2;
			}
			Steps.push_back({ (uint32_t)frame, port, (uint8_t)(*p == '-' ? 0 : ParseButtons(p)) });
		}
		std::cout << "input: " << Steps.size() << " steps from " << path << "\n";
		return true;
	}

	bool SetInput(BaseControlDevice* device) override
	{
		/* Mesen sends frame N at scanline 240 and polls input at scanline 241,
		 * so at the poll Frame already reads N; the host applies its step N+1
		 * at the same point (the end of its frame N, just before the NMI).
		 * Host frame N is Mesen frame N+K, K being how many frames later the
		 * program reaches the same state in Mesen - 0 for many programs, but a
		 * program's boot can take a frame longer in Mesen than in NESRecomp and
		 * the TriCNES oracle (SMB3: K = 1 at every CPU/PPU alignment, before
		 * any input). So Mesen takes the steps up to Frame + 1 - K. */
		uint32_t frame = Frame ? Frame->load() : 0;
		if(frame + 1 < FrameOffset) {
			return false;
		}
		uint32_t now = frame + 1 - FrameOffset;
		while(Next < Steps.size() && Steps[Next].Frame <= now) {
			Held[Steps[Next].Port] = Steps[Next].Buttons;
			Next++;
		}
		uint8_t port = device->GetPort();
		if(port > 1) {
			return false;
		}
		uint8_t b = Held[port];
		device->SetBitValue(NesController::Buttons::A, (b & 0x80) != 0);
		device->SetBitValue(NesController::Buttons::B, (b & 0x40) != 0);
		device->SetBitValue(NesController::Buttons::Select, (b & 0x20) != 0);
		device->SetBitValue(NesController::Buttons::Start, (b & 0x10) != 0);
		device->SetBitValue(NesController::Buttons::Up, (b & 0x08) != 0);
		device->SetBitValue(NesController::Buttons::Down, (b & 0x04) != 0);
		device->SetBitValue(NesController::Buttons::Left, (b & 0x02) != 0);
		device->SetBitValue(NesController::Buttons::Right, (b & 0x01) != 0);
		return true;
	}
};

/* ---- Mesen's opt-in hardware behaviors ----
 *
 * Mesen ships several real console behaviors switched off, as accuracy
 * options: OAM row corruption when rendering is toggled mid-screen, the sprite
 * evaluation bug, the $2000/$2006 scroll glitches and the DMC sample
 * duplication glitch. They are not uniformly faithful - Mesen's own source
 * calls its OAM row corruption "actually alignment-dependent, but we're
 * currently modeling worst-case behavior" - so which ones make it a better
 * reference depends on the program. --hw-options picks them: a comma-separated
 * list of oamrow, spriteeval, scroll2000, scroll2006, dmcdup, noppureset, or
 * all / none
 * (the default is none, Mesen's own default).
 *
 * OAM decay is never enabled: it depends on how long a console sits
 * unrefreshed, and NESRecomp does not model it. */
static bool ApplyHardwareOptions(NesConfig& cfg, const std::string& list)
{
	/* "all" means every hardware behavior Mesen can add. noppureset removes one,
	 * so it is only ever taken when named. */
	bool all = list == "all";
	auto has = [&](const char* name) {
		if(all) {
			return true;
		}
		size_t pos = 0;
		while(pos <= list.size()) {
			size_t end = list.find(',', pos);
			if(end == std::string::npos) {
				end = list.size();
			}
			if(list.compare(pos, end - pos, name) == 0) {
				return true;
			}
			pos = end + 1;
		}
		return false;
	};
	cfg.EnablePpuOamRowCorruption = has("oamrow");
	cfg.EnablePpuSpriteEvalBug = has("spriteeval");
	cfg.EnablePpu2000ScrollGlitch = has("scroll2000");
	cfg.EnablePpu2006ScrollGlitch = has("scroll2006");
	cfg.EnableDmcSampleDuplicationGlitch = has("dmcdup");
	/* The PPU ignoring $2000/$2001/$2005/$2006 writes until the end of the
	 * first frame after power-on is on by default in Mesen. It depends on the
	 * console model (front-loader NES, not Famicom or top-loader), and
	 * NESRecomp, like TriCNES, does not model it - so a program that enables
	 * NMIs that early starts a frame later in Mesen. "noppureset" turns it off. */
	cfg.DisablePpuReset = !all && has("noppureset");
	std::cout << "hw options: " << (list.empty() ? "none" : list) << "\n";
	return true;
}

class FrameShooter : public INotificationListener
{
public:
	Emulator* Emu = nullptr;
	std::string OutDir;
	std::set<uint32_t> Wanted;
	std::atomic<uint32_t> Frame{0};
	std::atomic<bool> Done{false};
	uint32_t Palette[512] = {};

	void ProcessNotification(ConsoleNotificationType type, void* parameter) override
	{
		if(type != ConsoleNotificationType::PpuFrameDone) {
			return;
		}
		uint32_t frame = ++Frame;
		if(Wanted.find(frame) != Wanted.end()) {
			/* The PPU's own output buffer, as RecordedRomTest::SaveFrame reads
			 * it: one 9-bit color index per pixel (color | emphasis << 6).
			 * The video decoder's TakeScreenshot goes through the renderer,
			 * which does not run in a headless build. */
			PpuFrameInfo info = Emu->GetPpuFrame();
			uint16_t* pixels = (uint16_t*)info.FrameBuffer;
			uint32_t count = info.Width * info.Height;

			/* Raw indices, for an exact comparison against another machine's
			 * picture without a palette in the way. */
			std::string base = OutDir + "/" + std::to_string(frame);
			std::ofstream idx(base + ".idx", std::ios::binary);
			idx.write((char*)pixels, (std::streamsize)count * 2);
			idx.close();

			std::vector<uint32_t> argb(count);
			for(uint32_t i = 0; i < count; i++) {
				argb[i] = Palette[pixels[i] & 0x1FF];
			}
			PNGHelper::WritePNG(base + ".png", argb.data(), info.Width, info.Height);

			std::cout << "frame " << frame << " (" << info.Width << "x" << info.Height << ") -> "
			          << base << ".png\n";
			std::cout.flush();
		}
		if(!Wanted.empty() && frame >= *Wanted.rbegin()) {
			Done = true;
		}
	}
};

int main(int argc, char** argv)
{
	if(argc < 4) {
		std::cerr << "usage: mesen_shot <rom.nes> <out-dir> [--input FILE] [--frame-offset K] [--hw-options LIST] "
		             "<frame> [frame ...]\n";
		return 2;
	}

	std::string rom = argv[1];
	auto shooter = std::make_shared<FrameShooter>();
	shooter->OutDir = argv[2];
	std::string inputFile;
	std::string hwOptions;
	uint32_t frameOffset = 0;
	for(int i = 3; i < argc; i++) {
		if(!strcmp(argv[i], "--input") && i + 1 < argc) {
			inputFile = argv[++i];
		} else if(!strcmp(argv[i], "--hw-options") && i + 1 < argc) {
			hwOptions = argv[++i];
			if(hwOptions == "none") {
				hwOptions.clear();
			}
		} else if(!strcmp(argv[i], "--frame-offset") && i + 1 < argc) {
			frameOffset = (uint32_t)strtoul(argv[++i], nullptr, 10);
		} else {
			shooter->Wanted.insert((uint32_t)strtoul(argv[i], nullptr, 10));
		}
	}
	if(shooter->Wanted.empty()) {
		std::cerr << "no frames requested\n";
		return 2;
	}
	FolderUtilities::CreateFolder(shooter->OutDir);
	FolderUtilities::SetHomeFolder(shooter->OutDir + "/MesenHome");

	std::unique_ptr<Emulator> emu(new Emulator());
	emu->Initialize(false);
	shooter->Emu = emu.get();

	EmuSettings* settings = emu->GetSettings();
	settings->SetFlag(EmulationFlags::TestMode);
	settings->GetEmulationConfig().RunAheadFrames = 0;
	settings->GetNesConfig().RamPowerOnState = RamState::AllZeros;
	settings->GetNesConfig().RemoveSpriteLimit = false;
	settings->GetNesConfig().DisablePpu2004Reads = false;
	settings->GetNesConfig().Region = ConsoleRegion::Ntsc;
	settings->GetNesConfig().EnableOamDecay = false;
	ApplyHardwareOptions(settings->GetNesConfig(), hwOptions);
	/* ControllerConfig::Type defaults to None, so a headless build has no
	 * controller for BaseControlManager to poll and --input would do nothing.
	 * Both ports are standard pads, as cyc_host.c presents them. */
	settings->GetNesConfig().Port1.Type = ControllerType::NesController;
	settings->GetNesConfig().Port2.Type = ControllerType::NesController;

	memcpy(shooter->Palette, kPalette2C02, sizeof(kPalette2C02));
	NesDefaultVideoFilter::GenerateFullColorPalette(shooter->Palette, PpuModel::Ppu2C02);

	emu->GetNotificationManager()->RegisterNotificationListener(shooter);

	if(!emu->LoadRom((VirtualFile)rom, VirtualFile())) {
		std::cerr << "cannot load " << rom << "\n";
		emu->Release();
		return 1;
	}

	/* After LoadRom, so the console's control manager exists to register with. */
	auto input = std::make_shared<ScheduledInput>();
	if(!inputFile.empty()) {
		if(!input->Load(inputFile)) {
			emu->Release();
			return 2;
		}
		input->Frame = &shooter->Frame;
		input->FrameOffset = frameOffset;
		emu->RegisterInputProvider(input.get());
	}
	settings->SetFlag(EmulationFlags::MaximumSpeed);

	uint32_t last = 0;
	int stalled = 0;
	while(!shooter->Done) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		uint32_t now = shooter->Frame;
		stalled = (now == last) ? stalled + 1 : 0;
		last = now;
		if(stalled > 200) {   /* 10 s with no frame produced */
			std::cerr << "stalled at frame " << now << "\n";
			break;
		}
	}

	std::cout << "frames run: " << shooter->Frame << "\n";
	if(!inputFile.empty()) {
		emu->UnregisterInputProvider(input.get());
	}
	emu->Stop(false);
	emu->Release();
	return 0;
}
