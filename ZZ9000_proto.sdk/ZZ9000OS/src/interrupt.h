#include <xscugic.h>

#ifndef ZZ_INTERRUPT_H
#define ZZ_INTERRUPT_H

XScuGic* interrupt_get_intc();
int interrupt_configure();

#endif
