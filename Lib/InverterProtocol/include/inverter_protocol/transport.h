#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inverter_protocol/protocol.h"

#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * Abstract transport interface.
 *
 * The InverterProtocol core is packet-format only; it does not know whether
 * packets travel over UART, CAN, TCP, etc. A concrete transport implements
 * this interface and owns the framing/segmentation details.
 *
 * For UART the adapter performs COBS encoding/decoding and 0x00 delimiters.
 * For CAN/CAN-FD the adapter segments/reassembles packets into MTU frames.
 * ======================================================================== */

typedef struct ivp_transport {
    void* ctx;

    /* Send a complete packet buffer. Returns true on success. */
    bool (*send)(void* ctx, const uint8_t* packet, size_t len);

    /* Try to receive a complete packet into `buf`. Returns the number of
     * bytes written, 0 if no packet is available, or -1 on error. */
    int (*receive)(void* ctx, uint8_t* buf, size_t cap);
} ivp_transport_t;

static inline bool ivp_transport_send(ivp_transport_t* t, const uint8_t* packet, size_t len) {
    return t && t->send && t->send(t->ctx, packet, len);
}

static inline int ivp_transport_receive(ivp_transport_t* t, uint8_t* buf, size_t cap) {
    if (!t || !t->receive) return -1;
    return t->receive(t->ctx, buf, cap);
}

#ifdef __cplusplus
} /* extern "C" */
#endif
