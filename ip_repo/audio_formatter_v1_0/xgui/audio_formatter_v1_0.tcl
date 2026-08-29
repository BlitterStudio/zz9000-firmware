
# Loading additional proc with user specified bodies to compute parameter values.
source [file join [file dirname [file dirname [info script]]] gui/audio_formatter_v1_0.gtcl]

# Definitional proc to organize widgets for parameters.
proc init_gui { IPINST } {
  ipgui::add_param $IPINST -name "Component_Name"
  #Adding Page
  set Page_0 [ipgui::add_page $IPINST -name "Page 0"]

	set panel1 [ipgui::add_panel $IPINST -name panel1 -parent $Page_0 ]
	set tempPanel [ipgui::add_group $IPINST -name "Read Interface Options" -parent $panel1 -header_param "c_enable_mm2s"]

  ipgui::add_param $IPINST -name "C_INCLUDE_MM2S" -parent ${tempPanel} -widget checkBox
  ipgui::add_param $IPINST -name "C_MAX_NUM_CHANNELS_MM2S" -parent ${tempPanel} -widget comboBox
  set C_PACKING_MODE_MM2S [ipgui::add_param $IPINST -name "C_PACKING_MODE_MM2S" -parent ${tempPanel} -widget comboBox]
  set_property tooltip {0: Interleaved 1: Non-Interleaved} ${C_PACKING_MODE_MM2S}
  set C_MM2S_DATAFORMAT [ipgui::add_param $IPINST -name "C_MM2S_DATAFORMAT" -parent ${tempPanel} -widget comboBox]
  set_property tooltip {0: AES -> AES 1: AES -> PCM  2: PCM -> PCM  3: PCM -> AES} ${C_MM2S_DATAFORMAT}
  set C_MM2S_ADDR_WIDTH [ipgui::add_param $IPINST -name "C_MM2S_ADDR_WIDTH" -parent ${tempPanel}]
  set_property tooltip {Memory Map Address Width} ${C_MM2S_ADDR_WIDTH}
  set C_MM2S_ASYNC_CLOCK [ipgui::add_param $IPINST -name "C_MM2S_ASYNC_CLOCK" -parent ${tempPanel} -widget checkBox]
  set_property tooltip {0: Clocks are Synchronous 1: Clocks are Asynchrnous} ${C_MM2S_ASYNC_CLOCK}

	set panel2 [ipgui::add_panel $IPINST -name panel2 -parent $Page_0 ]
	set tempPanel1 [ipgui::add_group $IPINST -name "Write Interface Options" -parent $panel2 -header_param "c_enable_s2mm"]

  ipgui::add_param $IPINST -name "C_INCLUDE_S2MM" -parent ${tempPanel1} -widget checkBox
  ipgui::add_param $IPINST -name "C_MAX_NUM_CHANNELS_S2MM" -parent ${tempPanel1} -widget comboBox
  set C_PACKING_MODE_S2MM [ipgui::add_param $IPINST -name "C_PACKING_MODE_S2MM" -parent ${tempPanel1} -widget comboBox]
  set_property tooltip {0: Interleaved 1: Non-Interleaved} ${C_PACKING_MODE_S2MM}
  set C_S2MM_DATAFORMAT [ipgui::add_param $IPINST -name "C_S2MM_DATAFORMAT" -parent ${tempPanel1} -widget comboBox]
  set_property tooltip {0: AES -> AES 1: AES -> PCM  2: PCM -> PCM} ${C_S2MM_DATAFORMAT}
  set C_S2MM_ADDR_WIDTH [ipgui::add_param $IPINST -name "C_S2MM_ADDR_WIDTH" -parent ${tempPanel1}]
  set_property tooltip {Memory Map Address Width} ${C_S2MM_ADDR_WIDTH}
  set C_S2MM_ASYNC_CLOCK [ipgui::add_param $IPINST -name "C_S2MM_ASYNC_CLOCK" -parent ${tempPanel1} -widget checkBox]
  set_property tooltip {0: Clocks are Synchronous 1: Clocks are Asynchrnous} ${C_S2MM_ASYNC_CLOCK}

}

proc update_PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S { PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_MAX_NUM_CHANNELS_MM2S when any of the dependent parameters in the arguments change
	
	set C_MAX_NUM_CHANNELS_MM2S ${PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S}
	set C_INCLUDE_MM2S ${PARAM_VALUE.C_INCLUDE_MM2S}
	set values(C_INCLUDE_MM2S) [get_property value $C_INCLUDE_MM2S]
	if { [gen_USERPARAMETER_C_MAX_NUM_CHANNELS_MM2S_ENABLEMENT $values(C_INCLUDE_MM2S)] } {
		set_property enabled true $C_MAX_NUM_CHANNELS_MM2S
	} else {
		set_property enabled false $C_MAX_NUM_CHANNELS_MM2S
		set_property value 2 $C_MAX_NUM_CHANNELS_MM2S
	}
}

proc validate_PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S { PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S } {
	# Procedure called to validate C_MAX_NUM_CHANNELS_MM2S
	return true
}

proc update_PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM { PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_MAX_NUM_CHANNELS_S2MM when any of the dependent parameters in the arguments change
	
	set C_MAX_NUM_CHANNELS_S2MM ${PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM}
	set C_INCLUDE_S2MM ${PARAM_VALUE.C_INCLUDE_S2MM}
	set values(C_INCLUDE_S2MM) [get_property value $C_INCLUDE_S2MM]
	if { [gen_USERPARAMETER_C_MAX_NUM_CHANNELS_S2MM_ENABLEMENT $values(C_INCLUDE_S2MM)] } {
		set_property enabled true $C_MAX_NUM_CHANNELS_S2MM
	} else {
		set_property enabled false $C_MAX_NUM_CHANNELS_S2MM
		set_property value 2 $C_MAX_NUM_CHANNELS_S2MM
	}
}

proc validate_PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM { PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM } {
	# Procedure called to validate C_MAX_NUM_CHANNELS_S2MM
	return true
}

proc update_PARAM_VALUE.C_MM2S_DATAFORMAT { PARAM_VALUE.C_MM2S_DATAFORMAT PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_MM2S_DATAFORMAT when any of the dependent parameters in the arguments change
	
	set C_MM2S_DATAFORMAT ${PARAM_VALUE.C_MM2S_DATAFORMAT}
	set C_INCLUDE_MM2S ${PARAM_VALUE.C_INCLUDE_MM2S}
	set values(C_INCLUDE_MM2S) [get_property value $C_INCLUDE_MM2S]
	if { [gen_USERPARAMETER_C_MM2S_DATAFORMAT_ENABLEMENT $values(C_INCLUDE_MM2S)] } {
		set_property enabled true $C_MM2S_DATAFORMAT
	} else {
		set_property enabled false $C_MM2S_DATAFORMAT
		set_property value 3 $C_MM2S_DATAFORMAT
	}
}

proc validate_PARAM_VALUE.C_MM2S_DATAFORMAT { PARAM_VALUE.C_MM2S_DATAFORMAT } {
	# Procedure called to validate C_MM2S_DATAFORMAT
	return true
}

proc update_PARAM_VALUE.C_PACKING_MODE_MM2S { PARAM_VALUE.C_PACKING_MODE_MM2S PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_PACKING_MODE_MM2S when any of the dependent parameters in the arguments change
	
	set C_PACKING_MODE_MM2S ${PARAM_VALUE.C_PACKING_MODE_MM2S}
	set C_INCLUDE_MM2S ${PARAM_VALUE.C_INCLUDE_MM2S}
	set values(C_INCLUDE_MM2S) [get_property value $C_INCLUDE_MM2S]
	if { [gen_USERPARAMETER_C_PACKING_MODE_MM2S_ENABLEMENT $values(C_INCLUDE_MM2S)] } {
		set_property enabled true $C_PACKING_MODE_MM2S
	} else {
		set_property enabled false $C_PACKING_MODE_MM2S
		set_property value 0 $C_PACKING_MODE_MM2S
	}
}

proc validate_PARAM_VALUE.C_PACKING_MODE_MM2S { PARAM_VALUE.C_PACKING_MODE_MM2S } {
	# Procedure called to validate C_PACKING_MODE_MM2S
	return true
}

proc update_PARAM_VALUE.C_MM2S_ASYNC_CLOCK { PARAM_VALUE.C_MM2S_ASYNC_CLOCK PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_MM2S_ASYNC_CLOCK when any of the dependent parameters in the arguments change
	
	set C_MM2S_ASYNC_CLOCK ${PARAM_VALUE.C_MM2S_ASYNC_CLOCK}
	set C_INCLUDE_MM2S ${PARAM_VALUE.C_INCLUDE_MM2S}
	set values(C_INCLUDE_MM2S) [get_property value $C_INCLUDE_MM2S]
	if { [gen_USERPARAMETER_C_PACKING_MODE_MM2S_ENABLEMENT $values(C_INCLUDE_MM2S)] } {
		set_property enabled true $C_MM2S_ASYNC_CLOCK
	} else {
		set_property enabled false $C_MM2S_ASYNC_CLOCK
		set_property value 1 $C_MM2S_ASYNC_CLOCK
	}
}

proc validate_PARAM_VALUE.C_MM2S_ASYNC_CLOCK { PARAM_VALUE.C_MM2S_ASYNC_CLOCK } {
	# Procedure called to validate C_MM2S_ASYNC_CLOCK
	return true
}

proc update_PARAM_VALUE.C_S2MM_ASYNC_CLOCK { PARAM_VALUE.C_S2MM_ASYNC_CLOCK PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_S2MM_ASYNC_CLOCK when any of the dependent parameters in the arguments change
	
	set C_S2MM_ASYNC_CLOCK ${PARAM_VALUE.C_S2MM_ASYNC_CLOCK}
	set C_INCLUDE_S2MM ${PARAM_VALUE.C_INCLUDE_S2MM}
	set values(C_INCLUDE_S2MM) [get_property value $C_INCLUDE_S2MM]
	if { [gen_USERPARAMETER_C_PACKING_MODE_S2MM_ENABLEMENT $values(C_INCLUDE_S2MM)] } {
		set_property enabled true $C_S2MM_ASYNC_CLOCK
	} else {
		set_property enabled false $C_S2MM_ASYNC_CLOCK
		set_property value 1 $C_S2MM_ASYNC_CLOCK
	}
}

proc validate_PARAM_VALUE.C_S2MM_ASYNC_CLOCK { PARAM_VALUE.C_S2MM_ASYNC_CLOCK } {
	# Procedure called to validate C_S2MM_ASYNC_CLOCK
	return true
}



proc update_PARAM_VALUE.C_PACKING_MODE_S2MM { PARAM_VALUE.C_PACKING_MODE_S2MM PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_PACKING_MODE_S2MM when any of the dependent parameters in the arguments change
	
	set C_PACKING_MODE_S2MM ${PARAM_VALUE.C_PACKING_MODE_S2MM}
	set C_INCLUDE_S2MM ${PARAM_VALUE.C_INCLUDE_S2MM}
	set values(C_INCLUDE_S2MM) [get_property value $C_INCLUDE_S2MM]
	if { [gen_USERPARAMETER_C_PACKING_MODE_S2MM_ENABLEMENT $values(C_INCLUDE_S2MM)] } {
		set_property enabled true $C_PACKING_MODE_S2MM
	} else {
		set_property enabled false $C_PACKING_MODE_S2MM
		set_property value 0 $C_PACKING_MODE_S2MM
	}
}

proc validate_PARAM_VALUE.C_PACKING_MODE_S2MM { PARAM_VALUE.C_PACKING_MODE_S2MM } {
	# Procedure called to validate C_PACKING_MODE_S2MM
	return true
}

proc update_PARAM_VALUE.C_S2MM_DATAFORMAT { PARAM_VALUE.C_S2MM_DATAFORMAT PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_S2MM_DATAFORMAT when any of the dependent parameters in the arguments change
	
	set C_S2MM_DATAFORMAT ${PARAM_VALUE.C_S2MM_DATAFORMAT}
	set C_INCLUDE_S2MM ${PARAM_VALUE.C_INCLUDE_S2MM}
	set values(C_INCLUDE_S2MM) [get_property value $C_INCLUDE_S2MM]
	if { [gen_USERPARAMETER_C_S2MM_DATAFORMAT_ENABLEMENT $values(C_INCLUDE_S2MM)] } {
		set_property enabled true $C_S2MM_DATAFORMAT
	} else {
		set_property enabled false $C_S2MM_DATAFORMAT
		set_property value 1 $C_S2MM_DATAFORMAT
	}
}

proc validate_PARAM_VALUE.C_S2MM_DATAFORMAT { PARAM_VALUE.C_S2MM_DATAFORMAT } {
	# Procedure called to validate C_S2MM_DATAFORMAT
	return true
}

proc update_PARAM_VALUE.C_S2MM_ADDR_WIDTH { PARAM_VALUE.C_S2MM_ADDR_WIDTH PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_S2MM_ADDR_WIDTH when any of the dependent parameters in the arguments change
	set C_S2MM_ADDR_WIDTH ${PARAM_VALUE.C_S2MM_ADDR_WIDTH}
	set C_INCLUDE_S2MM ${PARAM_VALUE.C_INCLUDE_S2MM}
	set values(C_INCLUDE_S2MM) [get_property value $C_INCLUDE_S2MM]
	if { [gen_USERPARAMETER_C_S2MM_ADDR_WIDTH_ENABLEMENT $values(C_INCLUDE_S2MM)] } {
		set_property enabled true $C_S2MM_ADDR_WIDTH
	} else {
		set_property enabled false $C_S2MM_ADDR_WIDTH
		set_property value 64 $C_S2MM_ADDR_WIDTH
	}
}

proc validate_PARAM_VALUE.C_S2MM_ADDR_WIDTH { PARAM_VALUE.C_S2MM_ADDR_WIDTH } {
	# Procedure called to validate C_S2MM_ADDR_WIDTH
	return true
}
proc update_PARAM_VALUE.C_MM2S_ADDR_WIDTH { PARAM_VALUE.C_MM2S_ADDR_WIDTH PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_MM2S_ADDR_WIDTH when any of the dependent parameters in the arguments change
	set C_MM2S_ADDR_WIDTH ${PARAM_VALUE.C_MM2S_ADDR_WIDTH}
	set C_INCLUDE_MM2S ${PARAM_VALUE.C_INCLUDE_MM2S}
	set values(C_INCLUDE_MM2S) [get_property value $C_INCLUDE_MM2S]
	if { [gen_USERPARAMETER_C_MM2S_ADDR_WIDTH_ENABLEMENT $values(C_INCLUDE_MM2S)] } {
		set_property enabled true $C_MM2S_ADDR_WIDTH
	} else {
		set_property enabled false $C_MM2S_ADDR_WIDTH
		set_property value 64 $C_MM2S_ADDR_WIDTH
	}
}

proc validate_PARAM_VALUE.C_MM2S_ADDR_WIDTH { PARAM_VALUE.C_MM2S_ADDR_WIDTH } {
	# Procedure called to validate C_MM2S_ADDR_WIDTH
	return true
}

proc update_PARAM_VALUE.C_INCLUDE_MM2S { PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to update C_INCLUDE_MM2S when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.C_INCLUDE_MM2S { PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to validate C_INCLUDE_MM2S
	return true
}

proc update_PARAM_VALUE.C_INCLUDE_S2MM { PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to update C_INCLUDE_S2MM when any of the dependent parameters in the arguments change
}

proc validate_PARAM_VALUE.C_INCLUDE_S2MM { PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to validate C_INCLUDE_S2MM
	return true
}


proc update_MODELPARAM_VALUE.C_INCLUDE_S2MM { MODELPARAM_VALUE.C_INCLUDE_S2MM PARAM_VALUE.C_INCLUDE_S2MM } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_INCLUDE_S2MM}] ${MODELPARAM_VALUE.C_INCLUDE_S2MM}
}

proc update_MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM { MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM}] ${MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_S2MM}
}

proc update_MODELPARAM_VALUE.C_PACKING_MODE_S2MM { MODELPARAM_VALUE.C_PACKING_MODE_S2MM PARAM_VALUE.C_PACKING_MODE_S2MM } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_PACKING_MODE_S2MM}] ${MODELPARAM_VALUE.C_PACKING_MODE_S2MM}
}

proc update_MODELPARAM_VALUE.C_S2MM_DATAFORMAT { MODELPARAM_VALUE.C_S2MM_DATAFORMAT PARAM_VALUE.C_S2MM_DATAFORMAT } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_S2MM_DATAFORMAT}] ${MODELPARAM_VALUE.C_S2MM_DATAFORMAT}
}

proc update_MODELPARAM_VALUE.C_INCLUDE_MM2S { MODELPARAM_VALUE.C_INCLUDE_MM2S PARAM_VALUE.C_INCLUDE_MM2S } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_INCLUDE_MM2S}] ${MODELPARAM_VALUE.C_INCLUDE_MM2S}
}

proc update_MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S { MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S}] ${MODELPARAM_VALUE.C_MAX_NUM_CHANNELS_MM2S}
}

proc update_MODELPARAM_VALUE.C_PACKING_MODE_MM2S { MODELPARAM_VALUE.C_PACKING_MODE_MM2S PARAM_VALUE.C_PACKING_MODE_MM2S } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_PACKING_MODE_MM2S}] ${MODELPARAM_VALUE.C_PACKING_MODE_MM2S}
}

proc update_MODELPARAM_VALUE.C_MM2S_ASYNC_CLOCK { MODELPARAM_VALUE.C_MM2S_ASYNC_CLOCK PARAM_VALUE.C_MM2S_ASYNC_CLOCK } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_MM2S_ASYNC_CLOCK}] ${MODELPARAM_VALUE.C_MM2S_ASYNC_CLOCK}
}

proc update_MODELPARAM_VALUE.C_S2MM_ASYNC_CLOCK { MODELPARAM_VALUE.C_S2MM_ASYNC_CLOCK PARAM_VALUE.C_S2MM_ASYNC_CLOCK } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_S2MM_ASYNC_CLOCK}] ${MODELPARAM_VALUE.C_S2MM_ASYNC_CLOCK}
}

proc update_MODELPARAM_VALUE.C_MM2S_DATAFORMAT { MODELPARAM_VALUE.C_MM2S_DATAFORMAT PARAM_VALUE.C_MM2S_DATAFORMAT } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_MM2S_DATAFORMAT}] ${MODELPARAM_VALUE.C_MM2S_DATAFORMAT}
}

proc update_MODELPARAM_VALUE.C_MM2S_ADDR_WIDTH { MODELPARAM_VALUE.C_MM2S_ADDR_WIDTH PARAM_VALUE.C_MM2S_ADDR_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_MM2S_ADDR_WIDTH}] ${MODELPARAM_VALUE.C_MM2S_ADDR_WIDTH}
}

proc update_MODELPARAM_VALUE.C_S2MM_ADDR_WIDTH { MODELPARAM_VALUE.C_S2MM_ADDR_WIDTH PARAM_VALUE.C_S2MM_ADDR_WIDTH } {
	# Procedure called to set VHDL generic/Verilog parameter value(s) based on TCL parameter value
	set_property value [get_property value ${PARAM_VALUE.C_S2MM_ADDR_WIDTH}] ${MODELPARAM_VALUE.C_S2MM_ADDR_WIDTH}
}

