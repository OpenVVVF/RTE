#pragma once

#include <cstdint>
#include <cstddef>

#include "fdcan.h"

namespace Inverter {

/**
 * @brief Dual-FDCAN bus driver: KV-gated enables, queued TX, mailbox RX.
 *
 * Bus numbering: 0 = FDCAN1 ("A", PA11/PA12, ROM-bootloader capable),
 *                1 = FDCAN2 ("B", PB12/PB6, Energica display).
 *
 * KV config (RteParamStore, applied at init; changes need reboot):
 *   Can.A.En (default 0), Can.B.En (default 1), Can.BitRate (default 500000).
 *
 * send() only queues (ISR-safe, drop-oldest on full); update() (main loop)
 * drains the queues into the TX FIFOs.  RX runs on the FIFO0 interrupt:
 * exact-ID mailboxes for graph nodes, a recent-frames ring for `can rxdump`,
 * and a sniffer hook for the protocol layer (stage 3).
 */
class CanBus {
public:
    static constexpr uint8_t NUM_BUSES = 2;

    struct Frame {
        uint32_t id;
        uint8_t  dlc;
        bool     ext;
        uint8_t  data[8];
    };

    typedef void (*RxHook)(uint8_t bus, const Frame& frame, void* user);

    bool init();
    void update();

    /** @brief Queue a frame for transmission.  Never blocks. */
    bool send(uint8_t bus, uint32_t id, bool ext, const uint8_t* data, uint8_t dlc);

    /**
     * @brief Latest received frame with an exact ID match.
     * First call for an ID subscribes its mailbox.  Returns false until a
     * frame with that ID has been received.  seqOut (optional) gets the
     * mailbox sequence counter (compare to detect "new since last step").
     */
    bool rxLatest(uint8_t bus, uint32_t id, Frame& out, uint32_t* seqOut = nullptr);

    /** @brief Free slots in the TX ring (main-loop pacing decisions). */
    size_t txFree(uint8_t bus) const;

    /** @brief Register the protocol-layer sniffer (one slot, stage 3). */
    void setRxHook(RxHook hook, void* user);

    bool   enabled(uint8_t bus) const;
    void   printStatus(uint8_t bus) const;
    void   printRecentRx(uint8_t bus) const;

    /** @brief HAL callback entry points (wired in CanBus.cpp). */
    void onRxFifo0(FDCAN_HandleTypeDef* h);

private:
    static constexpr size_t  TX_RING       = 16;
    static constexpr size_t  RX_MAILBOXES  = 16;
    static constexpr size_t  RX_RECENT     = 8;

    struct TxSlot {
        uint32_t id;
        uint8_t  dlc;
        bool     ext;
        uint8_t  data[8];
    };
    struct Mailbox {
        uint32_t id = 0;
        bool     used = false;
        Frame    frame = {};
        volatile uint32_t seq = 0;
    };

    FDCAN_HandleTypeDef* handle(uint8_t bus) const;
    bool applyTiming(uint8_t bus, uint32_t rate);
    void recoverIfBusOff(uint8_t bus);
    void storeRx(uint8_t bus, const Frame& f);

    bool     m_enabled[NUM_BUSES] = {false, false};
    uint32_t m_bitrate = 500000;

    TxSlot   m_tx[NUM_BUSES][TX_RING] = {};
    size_t   m_tx_head[NUM_BUSES] = {};
    size_t   m_tx_tail[NUM_BUSES] = {};

    Mailbox  m_mail[NUM_BUSES][RX_MAILBOXES] = {};
    Frame    m_recent[NUM_BUSES][RX_RECENT] = {};
    size_t   m_recent_head[NUM_BUSES] = {};

    RxHook   m_hook = nullptr;
    void*    m_hook_user = nullptr;

    uint32_t m_tx_frames[NUM_BUSES] = {};
    uint32_t m_tx_dropped[NUM_BUSES] = {};
    uint32_t m_rx_frames[NUM_BUSES] = {};
    uint32_t m_busoff_recoveries[NUM_BUSES] = {};
};

/** @brief Global CAN bus instance. */
CanBus& canBus();

} // namespace Inverter
