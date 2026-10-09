#ifndef TEST_USBD_CDC_IF_H
#define TEST_USBD_CDC_IF_H

#include "safety_fault_wire.h"
void CDC_Bridge_QueueSafetyStatus(const SafetyFaultFrame *frame);

#endif
