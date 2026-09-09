/*
 * File:           zz9000ax-mix1-lowpass-eq_IC_1.h
 *
 * Created:        Tuesday, 28 July 2026
 * Description:    ADAU1701 loader: symbols alias the post-mix
 *                 stereo-linked limiter graph tables.
 *
 * This software is distributed in the hope that it will be useful,
 * but is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * This software may only be used to program products purchased from
 * Analog Devices for incorporation by you into audio products that
 * are intended for resale to audio product end users. This software
 * may not be distributed whole or in any part to third parties.
 *
 * Copyright ©2026 Analog Devices, Inc. All rights reserved.
 */
#ifndef __ZZ9000AX_MIX1_IC_1_H__
#define __ZZ9000AX_MIX1_IC_1_H__

#define ADI_REG_TYPE unsigned char
#define REG_COREREGISTER_IC_1_BYTE 2

#define DEVICE_ARCHITECTURE_IC_1                  "ADAU1701"
#define DEVICE_ADDR_IC_1                          0x0

/* DSP Program Data: production ADC left/right -> DIG0/DIG1. */
#define PROGRAM_ADDR_NORMAL_ADC_IC_1 1024
/* Post-mix stereo-linked limiter graph: alias the loader symbols onto
 * the generated limiter tables (same sizes). */
#include "adau_limiter_image.h"
#define PROGRAM_SIZE_NORMAL_ADC_IC_1 PROGRAM_SIZE_LIMITER_IC_1
#define Program_Data_Normal_ADC_IC_1 Program_Data_Limiter_IC_1
#define PARAM_SIZE_NORMAL_ADC_IC_1 PARAM_SIZE_LIMITER_IC_1
#define Param_Data_Normal_ADC_IC_1 Param_Data_Limiter_IC_1

/* Register Default - IC 1.CoreRegister */
ADI_REG_TYPE R0_COREREGISTER_IC_1_Default[REG_COREREGISTER_IC_1_BYTE] = {
0x00, 0x18
};

/* Register Default - IC 1.HWConFiguration */
#define R3_HWCONFIGURATION_IC_1_SIZE 24
ADI_REG_TYPE R3_HWCONFIGURATION_IC_1_Default[R3_HWCONFIGURATION_IC_1_SIZE] = {
0x00, 0x18, 0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};

/* Register Default - IC 1.CoreRegister */
ADI_REG_TYPE R4_COREREGISTER_IC_1_Default[REG_COREREGISTER_IC_1_BYTE] = {
0x00, 0x1C
};

#endif
