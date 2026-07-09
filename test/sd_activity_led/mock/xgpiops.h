#ifndef XGPIOPS_H
#define XGPIOPS_H

#include <stdint.h>

typedef struct {
	uint32_t base_addr;
} XGpioPs;

typedef struct {
	uint16_t DeviceId;
	uint32_t BaseAddr;
} XGpioPs_Config;

XGpioPs_Config *XGpioPs_LookupConfig(uint16_t DeviceId);
int XGpioPs_CfgInitialize(XGpioPs *InstancePtr, XGpioPs_Config *ConfigPtr,
			  uint32_t EffectiveAddr);
void XGpioPs_SetDirectionPin(XGpioPs *InstancePtr, int Pin, int Direction);
void XGpioPs_SetOutputEnablePin(XGpioPs *InstancePtr, int Pin, int OpEnable);
void XGpioPs_WritePin(XGpioPs *InstancePtr, int Pin, int Data);

#endif /* XGPIOPS_H */
