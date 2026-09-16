// apu_harness.sv - NES_MiSTer's APU and DMA controller, wired as in its
// rtl/nes.v, with the CPU side driven from outside (cosim.cpp drives it from
// NESRecomp's machine, one CPU cycle at a time).
//
// Built by build.sh against a NES_MiSTer checkout: apu.sv and
// regs_savestates.sv are used as they are, DmaController is extracted from
// nes.v. Nothing from NES_MiSTer is copied into nesrecomp.
import regs_savestates::*;

module apu_harness (
	input  logic        clk,
	input  logic        reset,
	input  logic        cold_reset,
	input  logic        cpu_ce,       // nes.v: div_cpu == 12, the CPU's clock enable
	input  logic        phi2,         // nes.v: div_cpu 5-11
	input  logic        odd_or_even,  // nes.v: toggles every cpu_ce, 1 after reset
	input  logic [15:0] cpu_addr,     // the CPU's address and R/W this cycle
	input  logic        cpu_rnw,
	input  logic [7:0]  cpu_dout,     // the CPU's data output (writes)
	input  logic [7:0]  from_data_bus,// what the external bus reads at bus_addr
	// A DMC DMA read while the CPU holds $4000-$401F decodes the 2A03's
	// registers from the DMA address's low bits (nesdev wiki, DMA):
	input  logic        nesdev_conflict,// 1: a hit on $4015 gives the DMC the 2A03's internal read of
	                                    //    $4015, per nesdev; 0: the external bus, as nes.v wires it
	input  logic [7:0]  joypad_data,  // a hit on $4016/$4017: the controller port's value (the harness
	                                  // has no controllers; nes.v feeds its joypad read data here)
	input  logic [7:0]  internal_bus, // the 2A03's internal bus left by the last access ($4015 does
	                                  // not drive bit 5, so an internal read keeps it)
	output logic [15:0] bus_addr,     // address on the 2A03's external bus
	output logic [7:0]  apu_reg_value,// $4015 as the CPU would read it
	output logic        irq,
	output logic        dmc_ack,      // a DMC DMA reads the bus this cycle
	output logic        pause_cpu,
	output logic        sprite_dma,   // the sprite DMA owns the bus this cycle
	output logic [3:0]  sq1, sq2, tri_out, noise,
	output logic [6:0]  dmc,
	output logic [14:0] noise_shift,  // the noise LFSR (bit 14 out; NESRecomp's is mirrored)
	// DMC internals, for --show
	output logic [15:0] dmc_addr,
	output logic [11:0] dmc_bytes,
	output logic [7:0]  dmc_buffer, dmc_shift,
	output logic [2:0]  dmc_bits,
	output logic        dmc_have_buffer, dmc_silence, dmc_enable, dmc_req
);
	wire [15:0] dma_aout;
	wire        dma_aout_enable, dma_read;
	wire [7:0]  dma_data_to_ram;
	wire        apu_dma_request, apu_dma_ack;
	wire [15:0] apu_dma_addr;
	wire        get_ce, put_ce;
	wire [7:0]  apu_dout;
	wire [15:0] sample_apu;
	wire [63:0] ss_unused;

	wire apu_cs = cpu_addr[15:5] == 11'b0100_0000_000;
	wire [15:0] addr = dma_aout_enable ? dma_aout : cpu_addr;
	wire apu_reg_cs = apu_cs && addr[4:0] == 5'h15;
	assign apu_reg_value = {apu_dout[7:6], from_data_bus[5], apu_dout[4:0]};
	wire [7:0] internal_data_bus = apu_reg_cs ? apu_reg_value : from_data_bus;
	wire [7:0] dbus = dma_aout_enable ? dma_data_to_ram : cpu_dout;
	wire joypad_cs = apu_cs && (addr[4:0] == 5'h16 || addr[4:0] == 5'h17) && dma_aout_enable;
	wire [7:0] dma_data_bus = joypad_cs ? joypad_data :
		(nesdev_conflict && apu_reg_cs && dma_aout_enable) ? {apu_dout[7:6], internal_bus[5], apu_dout[4:0]} :
		from_data_bus;

	DmaController dma (
		.clk             (clk),
		.ce              (cpu_ce),
		.reset           (reset),
		.put_cycle       (odd_or_even),
		.sprite_trigger  (apu_cs && addr[4:0] == 5'h14 && ~cpu_rnw),
		.dmc_trigger     (apu_dma_request),
		.cpu_read        (cpu_rnw),
		.data_from_cpu   (cpu_dout),
		.dma_data_to_ram (internal_data_bus),
		.dmc_dma_addr    (apu_dma_addr),
		.aout            (dma_aout),
		.aout_enable     (dma_aout_enable),
		.read            (dma_read),
		.data_to_ram     (dma_data_to_ram),
		.dmc_ack         (apu_dma_ack),
		.pause_cpu       (pause_cpu),
		.get_ce          (get_ce),
		.put_ce          (put_ce)
	);

	APU #(.SSREG_INDEX_TOP(SSREG_INDEX_APU_TOP), .SSREG_INDEX_DMC1(SSREG_INDEX_APU_DMC1),
	      .SSREG_INDEX_DMC2(SSREG_INDEX_APU_DMC2), .SSREG_INDEX_FCT(SSREG_INDEX_APU_FCT)) apu (
		.MMC5              (1'b0),
		.clk               (clk),
		.PHI2              (phi2),
		.ce                (cpu_ce),
		.reset             (reset),
		.cold_reset        (cold_reset),
		.allow_us          (1'b0),
		.PAL               (1'b0),
		.ADDR              (addr[4:0]),
		.DIN               (dbus),
		.RW                (cpu_rnw),
		.CS                (apu_cs),
		.audio_channels    (5'b11111),
		.DmaData           (dma_data_bus),
		.get_or_put        (odd_or_even),
		.DmaAck            (apu_dma_ack),
		.DOUT              (apu_dout),
		.Sample            (sample_apu),
		.DmaReq            (apu_dma_request),
		.DmaAddr           (apu_dma_addr),
		.IRQ               (irq),
		.get_ce            (get_ce),
		.put_ce            (put_ce),
		.SaveStateBus_Din  (64'd0),
		.SaveStateBus_Adr  (10'd0),
		.SaveStateBus_wren (1'b0),
		.SaveStateBus_rst  (reset),
		.SaveStateBus_load (1'b0),
		.SaveStateBus_Dout (ss_unused)
	);

	assign bus_addr   = addr;
	assign dmc_ack    = apu_dma_ack;
	assign sprite_dma = dma_aout_enable && !apu_dma_ack;
	assign sq1        = apu.Sq1Sample;
	assign sq2        = apu.Sq2Sample;
	assign tri_out    = apu.TriSample;
	assign noise      = apu.NoiSample;
	assign dmc        = apu.DmcSample;
	assign noise_shift = apu.Noi.Shift;
	assign dmc_addr        = apu_dma_addr;
	assign dmc_bytes       = apu.Dmc.bytes_remaining;
	assign dmc_buffer      = apu.Dmc.sample_buffer;
	assign dmc_shift       = apu.Dmc.sample_shift;
	assign dmc_bits        = apu.Dmc.dmc_bits;
	assign dmc_have_buffer = apu.Dmc.have_buffer;
	assign dmc_silence     = apu.Dmc.dmc_silence;
	assign dmc_enable      = apu.Dmc.enable;
	assign dmc_req         = apu_dma_request;
endmodule
