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
 * Trace is an opt-in supplemental CAN-FD producer.  When Can.Trace.En=1,
 * only Can.Trace.Bus is switched to FD+BRS; classic traffic remains valid.
 *
 * send() only queues (ISR-safe, drop-newest on full). Classic-only buses keep
 * their original foreground drain; an FD trace bus uses low-priority IT1 so
 * foreground cadence cannot limit trace throughput. The FIFO0 interrupt copies
 * frames into a bounded software ring.  update() publishes exact-ID
 * mailboxes, the `can rxdump` history, and protocol-layer hooks so command
 * parsing and telemetry can never execute in interrupt context.
 */
class CanBus {
public:
    static constexpr uint8_t NUM_BUSES = 2;

    struct Frame {
        uint32_t id;
        uint8_t  dlc;
        bool     ext;
        uint8_t  data[64];
    };

    typedef void (*RxHook)(uint8_t bus, const Frame& frame, void* user);

    bool init();
    void update();

    /** @brief Queue a frame for transmission.  Never blocks. */
    bool send(uint8_t bus, uint32_t id, bool ext, const uint8_t* data, uint8_t dlc);

    /** @brief Queue one CAN-FD+BRS frame.  Reserved for the trace data plane. */
    bool sendFd(uint8_t bus, uint32_t id, const uint8_t* data, uint8_t length);

    /**
     * @brief Latest received frame with an exact ID match.
     * First call for an ID subscribes its mailbox.  Returns false until a
     * frame with that ID has been received.  seqOut (optional) gets the
     * mailbox sequence counter (compare to detect "new since last step").
     */
    bool rxLatest(uint8_t bus, uint32_t id, bool ext, Frame& out,
                  uint32_t* seqOut = nullptr);

    /** @brief Free slots in the TX ring (main-loop pacing decisions). */
    size_t txFree(uint8_t bus) const;

    /** @brief Register the protocol-layer sniffer (one slot, stage 3). */
    void setRxHook(RxHook hook, void* user);

    bool   enabled(uint8_t bus) const;
    bool   fdEnabled(uint8_t bus) const;
    void   printStatus(uint8_t bus) const;
    void   printRecentRx(uint8_t bus) const;

    /** @brief HAL callback entry point; captures frames only (wired in CanBus.cpp). */
    void onRxFifo0(FDCAN_HandleTypeDef* h);
    /** @brief Low-priority TX interrupt entry point; sole hardware TX consumer. */
    void serviceTx(FDCAN_HandleTypeDef* h);

private:
    static constexpr size_t  TX_RING       = 16;
    static constexpr size_t  FD_TX_RING    = 64;
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
    struct FdTxSlot {
        uint32_t id;
        uint8_t length;
        uint8_t data[64];
    };

    FDCAN_HandleTypeDef* handle(uint8_t bus) const;
    static bool validId(uint32_t id, bool ext);
    void resetState();
    bool applyTiming(uint8_t bus, uint32_t rate, uint32_t data_rate);
    void kickTx(uint8_t bus);
    void recoverIfBusOff(uint8_t bus);
    void processRx(uint8_t bus);
    void storeRx(uint8_t bus, const Frame& f);

    bool     m_enabled[NUM_BUSES] = {false, false};
    bool     m_fd_enabled[NUM_BUSES] = {false, false};
    uint32_t m_bitrate = 500000;
    uint32_t m_data_bitrate = 3000000;

    TxSlot   m_tx[NUM_BUSES][TX_RING] = {};
    volatile size_t m_tx_head[NUM_BUSES] = {};
    volatile size_t m_tx_tail[NUM_BUSES] = {};
    FdTxSlot m_fd_tx[NUM_BUSES][FD_TX_RING] = {};
    volatile size_t m_fd_tx_head[NUM_BUSES] = {};
    volatile size_t m_fd_tx_tail[NUM_BUSES] = {};

    /* RX frame storage lives in AXI SRAM in CanBus.cpp.  The ISR is the only
     * producer and update() is the only consumer. */
    volatile size_t m_rx_head[NUM_BUSES] = {};
    volatile size_t m_rx_tail[NUM_BUSES] = {};

    Mailbox  m_mail[NUM_BUSES][RX_MAILBOXES] = {};
    Frame    m_recent[NUM_BUSES][RX_RECENT] = {};
    size_t   m_recent_head[NUM_BUSES] = {};

    RxHook   m_hook = nullptr;
    void*    m_hook_user = nullptr;

    volatile uint32_t m_tx_frames[NUM_BUSES] = {};
    volatile uint32_t m_tx_dropped[NUM_BUSES] = {};
    volatile uint32_t m_fd_tx_frames[NUM_BUSES] = {};
    volatile uint32_t m_fd_tx_dropped[NUM_BUSES] = {};
    volatile uint32_t m_rx_frames[NUM_BUSES] = {};
    volatile uint32_t m_rx_queue_dropped[NUM_BUSES] = {};
    volatile uint32_t m_rx_malformed[NUM_BUSES] = {};
    volatile uint32_t m_rx_hal_errors[NUM_BUSES] = {};
    volatile uint32_t m_busoff_recoveries[NUM_BUSES] = {};
};

/** @brief Global CAN bus instance. */
CanBus& canBus();

} // namespace Inverter
