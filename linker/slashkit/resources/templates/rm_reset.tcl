# Shared user-RM reset spine, following the reference RM's reset replication.
# Keep the existing input register and polarity. The family registers add one
# user_clk cycle to both assertion and release, equally for every reset load.
# These are distribution registers, not CDC synchronizers. Do not route reset
# through BUFG_FABRIC or connect final loads directly to ilreduced_logic_0/Res.
set c_shift_ram_0 [create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 c_shift_ram_0]
set_property -dict [list CONFIG.Depth {1} CONFIG.Width {1}] $c_shift_ram_0

set ilreduced_logic_0 [create_bd_cell -type inline_hdl -vlnv xilinx.com:inline_hdl:ilreduced_logic:1.0 ilreduced_logic_0]
set_property -dict [list CONFIG.C_OPERATION {or} CONFIG.C_SIZE {1}] $ilreduced_logic_0

connect_bd_net [get_bd_ports arstn] [get_bd_pins c_shift_ram_0/D]
connect_bd_net [get_bd_pins c_shift_ram_0/Q] [get_bd_pins ilreduced_logic_0/Op1]

{% for family in ['hbm_sc', 'kernel', 'misc'] %}
set rst_repl_{{ family }} [create_bd_cell -type ip -vlnv xilinx.com:ip:c_shift_ram:12.0 rst_repl_{{ family }}]
set_property -dict [list CONFIG.Depth {1} CONFIG.Width {1}] $rst_repl_{{ family }}
connect_bd_net [get_bd_ports user_clk] [get_bd_pins rst_repl_{{ family }}/CLK]
connect_bd_net [get_bd_pins ilreduced_logic_0/Res] [get_bd_pins rst_repl_{{ family }}/D]
{% endfor %}
