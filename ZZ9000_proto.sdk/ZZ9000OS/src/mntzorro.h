#include "platform.h"
#include "memorymap.h"
#include "xil_io.h"

// FIXME!
#define MNTZ_BASE_ADDR 0x43C00000

#define MNTZORRO_REG0 0
#define MNTZORRO_REG1 4
#define MNTZORRO_REG2 8
#define MNTZORRO_REG3 12
#define MNTZORRO_REG4 16
#define MNTZORRO_REG5 20

#define mntzorro_read(BaseAddress, RegOffset) \
    Xil_In32((BaseAddress) + (RegOffset))

#define mntzorro_write(BaseAddress, RegOffset, Data) \
  	Xil_Out32((BaseAddress) + (RegOffset), (u32)(Data))


/*const char* zstates[53] = { "RESET   ", "Z2_CONF ", "Z2_IDLE ", "WAIT_WRI",
			"WAIT_WR2", "Z2WRIFIN", "WAIT_RD ", "WAIT_RD2", "WAIT_RD3",
			"CONFIGED", "CONF_CLR", "D_Z2_Z3 ", "Z3_IDLE ", "Z3_WRITE_UPP",
			"Z3_WRITE_LOW", "Z3_READ_UP", "Z3_READ_LOW", "Z3_READ_DLY",
			"Z3_READ_DLY1", "Z3_READ_DLY2", "Z3_WRITE_PRE", "Z3_WRITE_FIN",
			"Z3_ENDCYCLE", "Z3_DTACK", "Z3_CONFIG", "Z2_REGWRITE", "REGWRITE",
			"REGREAD", "Z2_REGR_POST", "Z3_REGR_POST", "Z3_REGWRITE",
			"Z2_REGREAD", "Z3_REGREAD", "NONE_33", "Z2_PRE_CONF", "Z2_ENDCYCLE",
			"NONE_36", "NONE_37", "NONE_38", "RESET_DVID", "COLD", "WR2B",
			"WR2C", "Z3DMA1", "Z3DMA2", "Z3_AUTOCONF_RD", "Z3_AUTOCONF_WR",
			"Z3_AUTOCONF_RD_DLY", "Z3_AUTOCONF_RD_DLY2", "Z3_REGWRITE_PRE",
			"Z3_REGREAD_PRE", "Z3_WRITE_PRE2", "UNDEF", };*/
