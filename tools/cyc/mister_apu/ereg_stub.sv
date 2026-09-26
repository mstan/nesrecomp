// ereg_stub.sv - SystemVerilog stand-in for NES_MiSTer's eReg_SavestateV
// (rtl/bus_savestates.vhd, VHDL, which Verilator does not read): a 64-bit
// register loaded with its default on BUS_rst and from BUS_Din when addressed.
// Savestates are never used by the harness, so the bus stays idle.
module eReg_SavestateV #(parameter [9:0] Adr = 10'd0, parameter [63:0] def = 64'd0) (
	input  logic        clk,
	input  logic [63:0] BUS_Din,
	input  logic [9:0]  BUS_Adr,
	input  logic        BUS_wren,
	input  logic        BUS_rst,
	output logic [63:0] BUS_Dout,
	input  logic [63:0] Din,
	output logic [63:0] Dout
);
	logic [63:0] buffer = def;
	always_ff @(posedge clk) begin
		if (BUS_rst) buffer <= def;
		else if (BUS_Adr == Adr && BUS_wren) buffer <= BUS_Din;
	end
	assign Dout = buffer;
	assign BUS_Dout = (BUS_Adr == Adr) ? Din : 64'd0;
endmodule
