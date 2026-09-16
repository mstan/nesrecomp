// cosim.cpp - NESRecomp's machine co-simulated with NES_MiSTer's APU.
//
// NESRecomp's CPU and hardware run the program (interpreter). After every CPU
// or DMA cycle (cyc_trace_cycle_hook) the CPU side of that cycle - address,
// R/W, written data, and the bus value for reads - drives MiSTer's APU and DMA
// controller (apu_harness.sv, Verilated) through one CPU cycle of its master
// clock, as its rtl/nes.v would. The two APUs never influence each other:
// MiSTer's only listens.
//
// Each signal is compared as a sequence of changes: both sides must produce
// the same values in the same order, and the cycle offset of each change
// (MiSTer's minus NESRecomp's) is tallied. A constant offset is a difference
// in where each model registers the signal; a drifting or varying one is a
// timing difference. Signals:
//   - the five channel outputs (pulse 1/2, triangle, noise 0-15; DMC 0-127),
//   - the IRQ output,
//   - DMC DMA sample reads and sprite DMA bus ownership,
// and $4015 as each CPU read of it sees it (bit 5 is open bus and masked).
//
//   cyc_mister_apu <rom.nes> [--frames N] [--acccoin] [--align N] [--phase 0|1]
//                  [--nes-v-conflicts] [--wav-ours FILE] [--wav-mister FILE]
//
// --phase sets MiSTer's get/put cycle phase at power-on relative to NESRecomp's
// (the console's is not defined). The WAV files mix each APU's channel levels
// with the same mixer, 48 kHz, so they differ only where the channels do.
//
// A DMC DMA that reads $xx15 while the CPU holds $4000-$401F makes the 2A03
// read $4015 internally and ignore the external bus (nesdev wiki, DMA); by
// default MiSTer's DMC is fed that way. --nes-v-conflicts feeds it the
// external bus instead, as NES_MiSTer's nes.v does.
#include "Vapu_harness.h"
#include "verilated.h"

extern "C" {
#include "cpu6502.h"
#include "cyc_accuracycoin.h"
#include "cyc_core.h"
#include "cyc_run.h"
#include "cyc_trace.h"
#include "hw_internal.h"
}

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

// Verilator's runtime asks the program for simulation time.
double sc_time_stamp() { return 0; }

enum { S_SQ1, S_SQ2, S_TRI, S_NOISE, S_DMC, S_IRQ, S_DMC_DMA, S_SPRITE_DMA, NSTREAMS };
static const char *stream_name[NSTREAMS] = {"pulse 1", "pulse 2", "triangle", "noise", "DMC level",
                                             "IRQ",     "DMC DMA read", "sprite DMA"};

// A change on one side waits this long for its counterpart before it counts
// as unmatched.
static const uint64_t MATCH_WINDOW = 40000;
static const int      SHOW = 8;

struct Event {
    uint64_t cycle;
    uint32_t value, prev;
};

struct Stream {
    uint32_t                    ours = 0, theirs = 0;
    bool                        started = false;
    std::deque<Event>           qo, qt;
    uint64_t                    matched = 0, mismatched = 0, unmatched_ours = 0, unmatched_theirs = 0;
    std::map<int64_t, uint64_t> offsets;
    std::map<int64_t, uint64_t> offset_first; // NESRecomp's cycle of each offset's first change
    std::vector<std::string>    notes;

    void note(const char *fmt, ...) {
        if ((int)notes.size() >= SHOW) return;
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        notes.emplace_back(buf);
    }

    void sample(uint64_t cycle, uint32_t o, uint32_t t) {
        if (!started) {
            started = true;
            ours = o;
            theirs = t;
            if (o != t) note("initial value: ours %u, MiSTer %u", o, t);
            return;
        }
        if (o != ours) qo.push_back({cycle, o, ours}), ours = o;
        if (t != theirs) qt.push_back({cycle, t, theirs}), theirs = t;
        while (!qo.empty() && !qt.empty()) {
            Event a = qo.front(), b = qt.front();
            qo.pop_front();
            qt.pop_front();
            if (a.value == b.value) {
                matched++;
                int64_t off = (int64_t)b.cycle - (int64_t)a.cycle;
                if (!offsets[off]++) offset_first[off] = a.cycle;
            } else {
                mismatched++;
                note("cycle %llu: ours %u->%u, MiSTer (cycle %llu) %u->%u", (unsigned long long)a.cycle, a.prev,
                     a.value, (unsigned long long)b.cycle, b.prev, b.value);
                // Resynchronize on the next changes.
                qo.clear();
                qt.clear();
                theirs = t;
                ours = o;
            }
        }
        while (!qo.empty() && cycle - qo.front().cycle > MATCH_WINDOW) {
            unmatched_ours++;
            note("cycle %llu: ours %u->%u, no MiSTer change within %llu cycles", (unsigned long long)qo.front().cycle,
                 qo.front().prev, qo.front().value, (unsigned long long)MATCH_WINDOW);
            qo.pop_front();
        }
        while (!qt.empty() && cycle - qt.front().cycle > MATCH_WINDOW) {
            unmatched_theirs++;
            note("cycle %llu: MiSTer %u->%u, no NESRecomp change within %llu cycles",
                 (unsigned long long)qt.front().cycle, qt.front().prev, qt.front().value,
                 (unsigned long long)MATCH_WINDOW);
            qt.pop_front();
        }
    }

    bool clean() const { return !mismatched && !unmatched_ours && !unmatched_theirs && offsets.size() <= 1; }
};

static VerilatedContext *ctx;
static Vapu_harness     *m;
static uint8_t           odd_or_even;
static uint8_t           last_dout;
static uint64_t          ncycles;
static Stream            streams[NSTREAMS];

static uint64_t                 show_from = UINT64_MAX, show_to = 0;
static uint64_t                 reads4015, mismatch4015;
static std::vector<std::string> notes4015;

// ---- noise LFSR ----
// The two LFSRs step the same 15-bit sequence mirrored (NESRecomp shifts
// right and outputs bit 0, MiSTer shifts left and outputs bit 14). Compared
// by the interval between clocks and by their distance along the sequence.
static uint16_t lfsr_pos[1 << 15], lfsr_state[32767];
static struct {
    uint16_t                     ours = 0xFFFF, theirs = 0xFFFF;
    uint64_t                     clock_ours = 0, clock_theirs = 0;
    std::map<uint64_t, uint64_t> interval_ours, interval_theirs;
    std::map<int, uint64_t>      distance;
} noise_cmp;

static void init_lfsr_positions() {
    uint16_t s = 1;
    for (int i = 0; i < 32767; i++) {
        lfsr_pos[s] = (uint16_t)i;
        lfsr_state[i] = s;
        unsigned fb = (s ^ (s >> 1)) & 1;
        s = (uint16_t)((s >> 1) | (fb << 14));
    }
}

static uint16_t mirror15(uint16_t x) {
    uint16_t r = 0;
    for (int i = 0; i < 15; i++)
        if (x & (1 << i)) r |= (uint16_t)(1 << (14 - i));
    return r;
}

static void compare_noise(uint16_t o, uint16_t t) {
    bool changed = false;
    if (o != noise_cmp.ours) {
        if (noise_cmp.ours != 0xFFFF) noise_cmp.interval_ours[ncycles - noise_cmp.clock_ours]++;
        noise_cmp.clock_ours = ncycles;
        noise_cmp.ours = o;
        changed = true;
    }
    if (t != noise_cmp.theirs) {
        if (noise_cmp.theirs != 0xFFFF) noise_cmp.interval_theirs[ncycles - noise_cmp.clock_theirs]++;
        noise_cmp.clock_theirs = ncycles;
        noise_cmp.theirs = t;
        changed = true;
    }
    if (changed && o && t) noise_cmp.distance[(lfsr_pos[o] - lfsr_pos[mirror15(t)] + 32767) % 32767]++;
}

static void print_top(const char *label, const std::map<uint64_t, uint64_t> &h) {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (auto &kv : h) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    printf("      %s:", label);
    for (size_t i = 0; i < v.size() && i < 6; i++) printf(" %llu cycles (x%llu)", (unsigned long long)v[i].second,
                                                      (unsigned long long)v[i].first);
    printf("\n");
}

// ---- audio ----

struct Wav {
    FILE    *f = nullptr;
    double   acc = 0, phase = 0;
    uint32_t count = 0, samples = 0;
    double   hp = 0, hp_in = 0;
};
static Wav          wav_ours, wav_theirs;
static const double CPU_CYCLES_PER_SAMPLE = 21477272.0 / 12.0 / 48000.0;

static void wav_header(Wav &w) {
    uint32_t data = w.samples * 2;
    uint8_t  h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
    uint32_t v[] = {36 + data, 48000, 96000};
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    h[32] = 2, h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    fseek(w.f, 0, SEEK_SET);
    fwrite(h, 1, 44, w.f);
    fseek(w.f, 0, SEEK_END);
}

static void wav_cycle(Wav &w, const uint8_t *lv) {
    if (!w.f) return;
    unsigned p = lv[0] + lv[1], t = 3 * lv[2] + 2 * lv[3] + lv[4];
    double level = (p ? 95.52 / (8128.0 / p + 100) : 0) + (t ? 163.67 / (24329.0 / t + 100) : 0);
    w.acc += level;
    w.count++;
    w.phase += 1.0;
    if (w.phase >= CPU_CYCLES_PER_SAMPLE) {
        w.phase -= CPU_CYCLES_PER_SAMPLE;
        double x = w.acc / w.count;
        w.acc = 0;
        w.count = 0;
        double y = 0.996 * (w.hp + x - w.hp_in); // DC blocker
        w.hp_in = x;
        w.hp = y;
        double s = y * 30000.0;
        int16_t pcm = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        fwrite(&pcm, 2, 1, w.f);
        w.samples++;
    }
}

// ---- MiSTer side ----

static uint8_t peek(uint16_t addr) {
    if (addr >= 0x8000) return hw_cart.prg[addr & (hw_cart.prg_len - 1)];
    if (addr < 0x2000) return hw.ram[addr & 0x7FF];
    return hw.data_bus;
}

// One master clock tick. nes.v's div_cpu runs 1-12: phi2 for 5-11, the CPU
// clock enable on 12. Returns the combinational outputs before the edge.
static void tick(int div_cpu) {
    m->cpu_ce = div_cpu == 12;
    m->phi2 = div_cpu > 4 && div_cpu < 12;
    m->odd_or_even = odd_or_even;
    m->clk = 0;
    m->eval();
    m->from_data_bus = peek(m->bus_addr);
    m->eval();
    m->clk = 1;
    m->eval();
}

static void reset_model(int phase) {
    m->reset = 1;
    m->cold_reset = 1;
    m->cpu_addr = 0;
    m->cpu_rnw = 1;
    m->cpu_dout = 0;
    odd_or_even = 1;
    for (int i = 0; i < 48; i++) tick(i % 12 + 1);
    m->reset = 0;
    m->cold_reset = 0;
    odd_or_even = (uint8_t)phase;
}

static void on_cycle(const CycTraceCycle *c) {
    // NESRecomp: after this cycle's access, before its APU clock (tick 0).
    uint8_t ours[NSTREAMS];
    apu_channel_levels(ours);
    ours[S_IRQ] = apu_irq_output();
    ours[S_DMC_DMA] = c->dma && !c->halt && c->kind == 'R' && c->addr >= 0x8000;
    ours[S_SPRITE_DMA] = c->dma && !c->halt && (c->kind == 'W' ? c->addr == 0x2004 : c->addr < 0x8000);
    wav_cycle(wav_ours, ours);

    // MiSTer: the CPU holds its address and reads while a DMA has the bus.
    bool cpu_access = !c->dma && c->kind != '-';
    m->cpu_addr = cpu_access ? c->addr : hw.cpu_addr;
    m->cpu_rnw = !(cpu_access && c->kind == 'W');
    if (cpu_access && c->kind == 'W') last_dout = c->value;
    m->cpu_dout = last_dout;
    // A DMA read hitting $4016/$4017 reads the controller port, which is
    // outside both APUs: MiSTer's DMC gets the value NESRecomp's port gave.
    m->joypad_data = c->value;
    // Bus history outside the APU comes from NESRecomp's machine, as open bus
    // does (peek); a $4015 read leaves bit 5 of it as it was.
    m->internal_bus = hw.internal_bus;
    for (int t = 1; t < 12; t++) tick(t);
    // The CPU latches a read on the cycle's last tick.
    m->cpu_ce = 1;
    m->phi2 = 0;
    m->clk = 0;
    m->eval();
    m->from_data_bus = peek(m->bus_addr);
    m->eval();
    uint8_t reg4015 = m->apu_reg_value, dmc_ack = m->dmc_ack, sprite = m->sprite_dma;
    tick(12);
    odd_or_even = !odd_or_even;

    uint8_t theirs[NSTREAMS] = {m->sq1, m->sq2, m->tri_out, m->noise, m->dmc, m->irq, dmc_ack, sprite};
    if (ncycles >= show_from && ncycles <= show_to) {
        // DMC internals: address, bytes left, buffer (+ = holds a byte), shifter,
        // bits (NESRecomp counts 8..1 left, MiSTer 0..7 done), s = silent.
        ApuDmcView d;
        apu_debug_dmc(&d);
        printf("%llu %c%s %04X %02X cpu=%04X%c | ours irq=%u dmc=%3u dmcdma=%u spr=%u [%04X %3u %02X%c %02X b%u%s%s t%u] "
               "| MiSTer irq=%u dmc=%3u ack=%u spr=%u pause=%u 4015=%02X [%04X %3u %02X%c %02X b%u%s%s%s]%s\n",
               (unsigned long long)ncycles, c->kind, c->dma ? (c->halt ? " halt" : " dma ") : "     ", c->addr,
               c->value, hw.cpu_addr, hw.cpu_reading ? 'r' : 'w', ours[S_IRQ], ours[S_DMC], ours[S_DMC_DMA],
               ours[S_SPRITE_DMA], d.addr, d.bytes, d.buffer, d.have_buffer ? '+' : ' ', d.shifter, d.bits,
               d.out_silent ? " s" : "", d.playing ? " on" : "", d.timer, m->irq, m->dmc, dmc_ack, sprite,
               m->pause_cpu, reg4015, m->dmc_addr, m->dmc_bytes, m->dmc_buffer, m->dmc_have_buffer ? '+' : ' ',
               m->dmc_shift, m->dmc_bits, m->dmc_silence ? " s" : "", m->dmc_enable ? " on" : "",
               m->dmc_req ? " req" : "", c->done ? " done" : "");
    }
    wav_cycle(wav_theirs, theirs);
    for (int s = 0; s < NSTREAMS; s++) streams[s].sample(ncycles, ours[s], theirs[s]);
    compare_noise(apu_noise_lfsr(), m->noise_shift);

    if (cpu_access && c->kind == 'R' && (c->addr & 0xFFE0) == 0x4000 && (c->addr & 0x1F) == 0x15) {
        reads4015++;
        if ((reg4015 & 0xDF) != (c->value & 0xDF) && mismatch4015++ < SHOW) {
            char buf[128];
            snprintf(buf, sizeof(buf), "cycle %llu: ours %02X, MiSTer %02X", (unsigned long long)ncycles, c->value,
                     reg4015);
            notes4015.emplace_back(buf);
        }
    }
    ncycles++;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    *size = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *rom = nullptr, *wav_o = nullptr, *wav_t = nullptr;
    long frames = 600;
    int align = 0, phase = 0, dmc_timer = -1, noise_back = -1, noise_timer = 0;
    bool acccoin = false, frames_given = false, nesv_conflicts = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]), frames_given = true;
        else if (!strcmp(argv[i], "--acccoin")) acccoin = true;
        else if (!strcmp(argv[i], "--align") && i + 1 < argc) align = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--phase") && i + 1 < argc) phase = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nes-v-conflicts")) nesv_conflicts = true;
        else if (!strcmp(argv[i], "--dmc-timer") && i + 1 < argc) dmc_timer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--noise-back") && i + 1 < argc) noise_back = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--noise-timer") && i + 1 < argc) noise_timer = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--show") && i + 2 < argc) show_from = strtoull(argv[i + 1], 0, 10), show_to = strtoull(argv[i + 2], 0, 10), i += 2;
        else if (!strcmp(argv[i], "--wav-ours") && i + 1 < argc) wav_o = argv[++i];
        else if (!strcmp(argv[i], "--wav-mister") && i + 1 < argc) wav_t = argv[++i];
        else if (argv[i][0] != '-' && !rom) rom = argv[i];
        else {
            fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom) {
        fprintf(stderr, "usage: %s <rom.nes> [--frames N] [--acccoin] [--align N] [--phase 0|1] [--nes-v-conflicts] [--dmc-timer N] [--noise-back N --noise-timer T] "
                        "[--wav-ours FILE] [--wav-mister FILE]\n", argv[0]);
        return 2;
    }
    size_t size;
    uint8_t *image = read_file(rom, &size);
    if (!image || !cyc_load_ines(image, size)) {
        fprintf(stderr, "cannot load %s\n", rom);
        return 2;
    }
    if (wav_o && (wav_ours.f = fopen(wav_o, "wb"))) wav_header(wav_ours);
    if (wav_t && (wav_theirs.f = fopen(wav_t, "wb"))) wav_header(wav_theirs);

    ctx = new VerilatedContext;
    m = new Vapu_harness{ctx};
    m->nesdev_conflict = !nesv_conflicts;
    reset_model(phase);

    init_lfsr_positions();
    cyc_power_on((uint8_t)align);
    if (dmc_timer >= 0) apu_debug_set_dmc_timer((uint16_t)dmc_timer);
    // Start NESRecomp's noise LFSR noise_back clocks before its power-on state.
    if (noise_back >= 0) apu_debug_set_noise(lfsr_state[(32767 - noise_back % 32767) % 32767], (uint16_t)noise_timer);
    cyc_run_power_on();
    apu_audio_enable(true, 48000);
    cyc_trace_enabled = true;
    cyc_trace_cycle_hook = on_cycle;

    AccCoinDriver drv;
    acccoin_driver_init(&drv);
    long frame = 0;
    int16_t discard[4096];
    for (;;) {
        if (acccoin && drv.done) break;
        if ((!acccoin || frames_given) && frame >= frames) break;
        if (acccoin) cyc_set_controller(0, acccoin_driver_tick(&drv, cyc_cpu_ram()));
        cyc_run_frame();
        while (apu_audio_read(discard, 4096)) {}
        frame++;
    }

    printf("frames=%ld cycles=%llu phase=%d align=%d\n", frame, (unsigned long long)ncycles, phase, align);
    bool ok = true;
    for (int s = 0; s < NSTREAMS; s++) {
        const Stream &st = streams[s];
        std::vector<std::pair<uint64_t, int64_t>> by_count;
        for (auto &kv : st.offsets) by_count.push_back({kv.second, kv.first});
        std::sort(by_count.rbegin(), by_count.rend());
        printf("  %-13s %llu changes matched", stream_name[s], (unsigned long long)st.matched);
        if (!by_count.empty()) {
            printf(", MiSTer's offset:");
            for (size_t i = 0; i < by_count.size() && i < 4; i++)
                printf(" %+lld (x%llu)", (long long)by_count[i].second, (unsigned long long)by_count[i].first);
            if (by_count.size() > 4) printf(" +%zu more offsets", by_count.size() - 4);
        }
        printf("; %llu mismatched, %llu/%llu unmatched (ours/MiSTer)\n", (unsigned long long)st.mismatched,
               (unsigned long long)st.unmatched_ours, (unsigned long long)st.unmatched_theirs);
        // Where the offsets other than the most common one first appear.
        for (size_t i = 1; i < by_count.size() && i < 1 + SHOW; i++)
            printf("      offset %+lld (x%llu) first at cycle %llu\n", (long long)by_count[i].second,
                   (unsigned long long)by_count[i].first, (unsigned long long)st.offset_first.at(by_count[i].second));
        for (auto &n : st.notes) printf("      %s\n", n.c_str());
        ok &= st.clean();
    }
    printf("  noise LFSR\n");
    print_top("NESRecomp clock intervals", noise_cmp.interval_ours);
    print_top("MiSTer clock intervals   ", noise_cmp.interval_theirs);
    {
        std::map<uint64_t, uint64_t> d;
        for (auto &kv : noise_cmp.distance) d[(uint64_t)kv.first] = kv.second;
        print_top("sequence distance (clocks) at changes", d);
    }
    printf("  $4015 reads   %llu, %llu differ\n", (unsigned long long)reads4015, (unsigned long long)mismatch4015);
    for (auto &n : notes4015) printf("      %s\n", n.c_str());
    ok &= mismatch4015 == 0;
    if (acccoin) {
        const uint8_t *prg = image + 16 + ((image[6] & 0x04) ? 512 : 0);
        acccoin_report(prg, (size_t)image[4] * 0x4000, cyc_cpu_ram(), stdout, false);
    }
    if (wav_ours.f) wav_header(wav_ours), fclose(wav_ours.f);
    if (wav_theirs.f) wav_header(wav_theirs), fclose(wav_theirs.f);
    m->final();
    return ok ? 0 : 1;
}
