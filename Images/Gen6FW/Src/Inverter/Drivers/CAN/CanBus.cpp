#include "Inverter/Drivers/CAN/CanBus.h"

#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include <cstring>

namespace Inverter {

namespace {
/* CAN transport state is not control-loop working state.  Keep its TX rings,
 * mailboxes, histories, and counters in roomy AXI SRAM instead of consuming
 * scarce DTCM needed by generated motor-control graphs. */
CanBus s_instance __attribute__((section(".dma_buffers")));

/* Capacity is 63 frames because one slot distinguishes full from empty.
 * Keep the payload out of DTCM: this is transport buffering, not control-loop
 * working state. */
constexpr size_t RX_RING = 64;
CanBus::Frame s_rx_ring[CanBus::NUM_BUSES][RX_RING]
    __attribute__((section(".dma_buffers")));

uint32_t irqSave() {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    return primask;
}

void irqRestore(uint32_t primask) {
    __DMB();
    __set_PRIMASK(primask);
}

float kvOr(const char* key, float def) {
    float v = def;
    if (RteParamStore::isReady()) {
        RteParamStore::get(key, &v);
    }
    return v;
}

/* FDCAN DLC field -> byte count (classic frames are 1:1 up to 8). */
uint8_t dlcToBytes(uint32_t dlc_field) {
    if (dlc_field <= 8U) return static_cast<uint8_t>(dlc_field);
    switch (dlc_field) {
        case FDCAN_DLC_BYTES_12: return 12;
        case FDCAN_DLC_BYTES_16: return 16;
        case FDCAN_DLC_BYTES_20: return 20;
        case FDCAN_DLC_BYTES_24: return 24;
        case FDCAN_DLC_BYTES_32: return 32;
        case FDCAN_DLC_BYTES_48: return 48;
        case FDCAN_DLC_BYTES_64: return 64;
        default: return 0;
    }
}

uint32_t bytesToDlc(uint8_t bytes) {
    if (bytes <= 8U) return bytes;
    switch (bytes) {
        case 12: return FDCAN_DLC_BYTES_12;
        case 16: return FDCAN_DLC_BYTES_16;
        case 20: return FDCAN_DLC_BYTES_20;
        case 24: return FDCAN_DLC_BYTES_24;
        case 32: return FDCAN_DLC_BYTES_32;
        case 48: return FDCAN_DLC_BYTES_48;
        case 64: return FDCAN_DLC_BYTES_64;
        default: return UINT32_MAX;
    }
}
} // namespace

CanBus& canBus() {
    return s_instance;
}

FDCAN_HandleTypeDef* CanBus::handle(uint8_t bus) const {
    return (bus == 0) ? &hfdcan1 : &hfdcan2;
}

bool CanBus::validId(uint32_t id, bool ext) {
    return ext ? id <= 0x1FFFFFFFU : id <= 0x7FFU;
}

void CanBus::resetState() {
    /* .dma_buffers is intentionally NOLOAD, so explicitly initialize every
     * field that controls whether buffered data is considered valid. */
    m_bitrate = 500000;
    m_data_bitrate = 3000000;
    m_hook = nullptr;
    m_hook_user = nullptr;
    for (uint8_t bus = 0; bus < NUM_BUSES; ++bus) {
        m_enabled[bus] = false;
        m_fd_enabled[bus] = false;
        m_tx_head[bus] = 0;
        m_tx_tail[bus] = 0;
        m_fd_tx_head[bus] = 0;
        m_fd_tx_tail[bus] = 0;
        m_rx_head[bus] = 0;
        m_rx_tail[bus] = 0;
        m_recent_head[bus] = 0;
        m_tx_frames[bus] = 0;
        m_tx_dropped[bus] = 0;
        m_fd_tx_frames[bus] = 0;
        m_fd_tx_dropped[bus] = 0;
        m_rx_frames[bus] = 0;
        m_rx_queue_dropped[bus] = 0;
        m_rx_malformed[bus] = 0;
        m_rx_hal_errors[bus] = 0;
        m_busoff_recoveries[bus] = 0;
        for (size_t i = 0; i < TX_RING; ++i) {
            m_tx[bus][i] = TxSlot{};
        }
        for (size_t i = 0; i < FD_TX_RING; ++i) {
            m_fd_tx[bus][i] = FdTxSlot{};
        }
        for (size_t i = 0; i < RX_MAILBOXES; ++i) {
            m_mail[bus][i] = Mailbox{};
        }
        for (size_t i = 0; i < RX_RECENT; ++i) {
            m_recent[bus][i] = Frame{};
        }
    }
}

bool CanBus::enabled(uint8_t bus) const {
    return bus < NUM_BUSES && m_enabled[bus];
}

bool CanBus::fdEnabled(uint8_t bus) const {
    return enabled(bus) && m_fd_enabled[bus];
}

size_t CanBus::txFree(uint8_t bus) const {
    if (!enabled(bus)) {
        return 0;
    }
    const size_t head = m_tx_head[bus];
    const size_t tail = m_tx_tail[bus];
    return (tail + TX_RING - head - 1) % TX_RING;
}

bool CanBus::applyTiming(uint8_t bus, uint32_t rate, uint32_t data_rate) {
    /* FDCAN kernel clock is 96 MHz (PLL2P).  Prefer a 24-tq bit
     * (seg1 17, seg2 6, sjw 4, ~75% sample point) and pick the prescaler
     * that lands within 1% of the requested rate. */
    uint32_t best_presc = 8;
    uint32_t best_err = UINT32_MAX;
    for (uint32_t presc = 1; presc <= 32; ++presc) {
        const uint32_t actual = 96000000U / (presc * 24U);
        const uint32_t err = (actual > rate) ? (actual - rate) : (rate - actual);
        if (err < best_err) {
            best_err = err;
            best_presc = presc;
        }
    }
    const uint32_t actual = 96000000U / (best_presc * 24U);
    if (best_err * 100U > rate) {  /* >1% off: refuse rather than mis-key */
        Telemetry::printf("[CAN] bus %u: no clean timing for %lu bit/s",
                          static_cast<unsigned>(bus),
                          static_cast<unsigned long>(rate));
        return false;
    }

    FDCAN_HandleTypeDef* h = handle(bus);
    (void)HAL_FDCAN_Stop(h);
    (void)HAL_FDCAN_DeInit(h);
    /* Normal CAN reliability requires retry after arbitration loss or a
     * transient transmit error.  Cube's generated defaults disable it. */
    h->Init.AutoRetransmission = ENABLE;
    const bool any_fd = m_fd_enabled[0] || m_fd_enabled[1];
    h->Init.MessageRAMOffset = any_fd ? ((bus == 0U) ? 1024U : 0U)
                                      : ((bus == 0U) ? 256U : 0U);
    h->Init.FrameFormat = m_fd_enabled[bus] ? FDCAN_FRAME_FD_BRS
                                            : FDCAN_FRAME_CLASSIC;
    h->Init.RxFifo0ElmtSize = m_fd_enabled[bus] ? FDCAN_DATA_BYTES_64
                                                : FDCAN_DATA_BYTES_8;
    h->Init.TxElmtSize = m_fd_enabled[bus] ? FDCAN_DATA_BYTES_64
                                           : FDCAN_DATA_BYTES_8;
    h->Init.NominalPrescaler = best_presc;
    h->Init.NominalSyncJumpWidth = 4;
    h->Init.NominalTimeSeg1 = 17;
    h->Init.NominalTimeSeg2 = 6;
    if (m_fd_enabled[bus]) {
        uint32_t best_data_presc = 0;
        uint32_t best_data_tq = 0;
        uint32_t best_data_err = UINT32_MAX;
        for (uint32_t presc = 1; presc <= 32; ++presc) {
            for (uint32_t tq = 5; tq <= 32; ++tq) {
                const uint32_t candidate = 96000000U / (presc * tq);
                const uint32_t err = candidate > data_rate
                    ? candidate - data_rate : data_rate - candidate;
                if (err < best_data_err) {
                    best_data_err = err;
                    best_data_presc = presc;
                    best_data_tq = tq;
                }
            }
        }
        if (best_data_presc == 0U || best_data_err * 100U > data_rate) {
            Telemetry::printf("[CAN] bus %u: no clean FD timing for %lu bit/s",
                              static_cast<unsigned>(bus),
                              static_cast<unsigned long>(data_rate));
            return false;
        }
        const uint32_t seg2 = best_data_tq / 4U < 2U ? 2U : best_data_tq / 4U;
        h->Init.DataPrescaler = best_data_presc;
        h->Init.DataSyncJumpWidth = seg2 < 4U ? seg2 : 4U;
        h->Init.DataTimeSeg1 = best_data_tq - seg2 - 1U;
        h->Init.DataTimeSeg2 = seg2;
    }
    if (HAL_FDCAN_Init(h) != HAL_OK) {
        return false;
    }
    Telemetry::printf("[CAN] bus %u: %lu bit/s%s (presc %lu, actual %lu)",
                      static_cast<unsigned>(bus),
                      static_cast<unsigned long>(rate),
                      m_fd_enabled[bus] ? ", FD+BRS" : "",
                      static_cast<unsigned long>(best_presc),
                      static_cast<unsigned long>(actual));
    return true;
}

bool CanBus::init() {
    resetState();
    m_enabled[0] = kvOr("Can.A.En", 0.0f) != 0.0f;
    m_enabled[1] = kvOr("Can.B.En", 1.0f) != 0.0f;
    const bool trace_enabled = kvOr("Can.Trace.En", 0.0f) != 0.0f;
    const float trace_bus = kvOr("Can.Trace.Bus", 1.0f);
    const float data_kbaud_config = kvOr("Can.Trace.DataKBaud", 3000.0f);
    if (trace_enabled && (trace_bus == 1.0f || trace_bus == 2.0f)) {
        m_fd_enabled[static_cast<uint8_t>(trace_bus) - 1U] = true;
    }
    const float bitrate_config = kvOr("Can.BitRate", 500000.0f);
    if (!(bitrate_config >= 10000.0f && bitrate_config <= 1000000.0f) ||
        bitrate_config != static_cast<float>(static_cast<uint32_t>(bitrate_config))) {
        Telemetry::printf("[CAN] invalid Can.BitRate; both buses disabled");
        m_enabled[0] = false;
        m_enabled[1] = false;
        return false;
    }
    m_bitrate = static_cast<uint32_t>(bitrate_config);
    if (!(data_kbaud_config >= 1000.0f && data_kbaud_config <= 5000.0f) ||
        data_kbaud_config != static_cast<float>(static_cast<uint32_t>(data_kbaud_config))) {
        Telemetry::printf("[CAN] invalid Can.Trace.DataKBaud; trace FD disabled");
        m_fd_enabled[0] = false;
        m_fd_enabled[1] = false;
    } else {
        m_data_bitrate = static_cast<uint32_t>(data_kbaud_config) * 1000U;
    }

    if (!m_enabled[0] && !m_enabled[1]) {
        Telemetry::printf("[CAN] both buses disabled (Can.A.En/Can.B.En)");
        return true;
    }

    /* Transceiver rail. */
    HAL_GPIO_WritePin(CANBUS_POWER_ENABLE_GPIO_Port,
                      CANBUS_POWER_ENABLE_Pin, GPIO_PIN_SET);

    for (uint8_t bus = 0; bus < NUM_BUSES; ++bus) {
        if (!m_enabled[bus]) {
            continue;
        }
        FDCAN_HandleTypeDef* h = handle(bus);
        if (!applyTiming(bus, m_bitrate, m_data_bitrate)) {
            Telemetry::printf("[CAN] ERROR: bus %u timing/init failed", bus);
            m_enabled[bus] = false;
            continue;
        }
        /* Accept everything into FIFO0 (mailbox/filtering is software-side
         * for now); reject remote frames. */
        if (HAL_FDCAN_ConfigGlobalFilter(h,
                FDCAN_ACCEPT_IN_RX_FIFO0, FDCAN_ACCEPT_IN_RX_FIFO0,
                FDCAN_REJECT_REMOTE, FDCAN_REJECT_REMOTE) != HAL_OK) {
            m_enabled[bus] = false;
            continue;
        }
        if (HAL_FDCAN_ActivateNotification(h, FDCAN_IT_RX_FIFO0_NEW_MESSAGE,
                                           0) != HAL_OK) {
            m_enabled[bus] = false;
            continue;
        }
        if (HAL_FDCAN_Start(h) != HAL_OK) {
            m_enabled[bus] = false;
            continue;
        }
        if (m_fd_enabled[bus]) {
            if (HAL_FDCAN_ConfigInterruptLines(h, FDCAN_IT_TX_FIFO_EMPTY,
                                               FDCAN_INTERRUPT_LINE1) != HAL_OK ||
                HAL_FDCAN_ActivateNotification(h, FDCAN_IT_TX_FIFO_EMPTY, 0) != HAL_OK) {
                m_enabled[bus] = false;
                continue;
            }
        }
    }

    /* RX FIFO0 + error interrupts share IT0 on each peripheral. */
    HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 6, 0);
    HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);
    HAL_NVIC_SetPriority(FDCAN1_IT1_IRQn, 12, 0);
    HAL_NVIC_SetPriority(FDCAN2_IT1_IRQn, 12, 0);
    if (m_fd_enabled[0] && m_enabled[0]) HAL_NVIC_EnableIRQ(FDCAN1_IT1_IRQn);
    if (m_fd_enabled[1] && m_enabled[1]) HAL_NVIC_EnableIRQ(FDCAN2_IT1_IRQn);

    return m_enabled[0] || m_enabled[1];
}

bool CanBus::send(uint8_t bus, uint32_t id, bool ext, const uint8_t* data,
                  uint8_t dlc) {
    if (!enabled(bus) || !validId(id, ext) || dlc > 8 ||
        (data == nullptr && dlc != 0)) {
        return false;
    }
    const uint32_t primask = irqSave();
    size_t next = (m_tx_head[bus] + 1) % TX_RING;
    if (next == m_tx_tail[bus]) {
        /* Full: reject (drop-newest).  Dropping the OLDEST would corrupt an
         * in-progress segmented protocol packet and waste the bus time of
         * its remaining chunks; rejecting new work keeps in-flight packets
         * intact. */
        ++m_tx_dropped[bus];
        irqRestore(primask);
        return false;
    }
    TxSlot& s = m_tx[bus][m_tx_head[bus]];
    s.id = id;
    s.ext = ext;
    s.dlc = dlc;
    if (dlc != 0) {
        std::memcpy(s.data, data, dlc);
    }
    m_tx_head[bus] = next;
    irqRestore(primask);
    if (fdEnabled(bus)) kickTx(bus);
    return true;
}

bool CanBus::sendFd(uint8_t bus, uint32_t id, const uint8_t* data, uint8_t length) {
    if (!fdEnabled(bus) || !validId(id, false) || data == nullptr ||
        bytesToDlc(length) == UINT32_MAX) return false;
    const uint32_t primask = irqSave();
    const size_t next = (m_fd_tx_head[bus] + 1U) % FD_TX_RING;
    if (next == m_fd_tx_tail[bus]) {
        ++m_fd_tx_dropped[bus];
        irqRestore(primask);
        return false;
    }
    FdTxSlot& slot = m_fd_tx[bus][m_fd_tx_head[bus]];
    slot.id = id;
    slot.length = length;
    std::memcpy(slot.data, data, length);
    m_fd_tx_head[bus] = next;
    irqRestore(primask);
    kickTx(bus);
    return true;
}

void CanBus::kickTx(uint8_t bus) {
    if (bus == 0U) NVIC_SetPendingIRQ(FDCAN1_IT1_IRQn);
    else NVIC_SetPendingIRQ(FDCAN2_IT1_IRQn);
}

void CanBus::update() {
    for (uint8_t bus = 0; bus < NUM_BUSES; ++bus) {
        if (!enabled(bus)) {
            continue;
        }
        processRx(bus);
        recoverIfBusOff(bus);

        if (fdEnabled(bus)) {
            kickTx(bus);
        } else {
            /* Preserve the original classic-CAN scheduling and behavior when
             * supplemental trace is disabled (the default). */
            serviceTx(handle(bus));
        }
    }
}

void CanBus::serviceTx(FDCAN_HandleTypeDef* h) {
    if (h == nullptr || (h->Instance != FDCAN1 && h->Instance != FDCAN2)) return;
    const uint8_t bus = h->Instance == FDCAN1 ? 0U : 1U;
    if (!enabled(bus)) return;
    while (HAL_FDCAN_GetTxFifoFreeLevel(h) > 0U) {
        if (m_tx_tail[bus] != m_tx_head[bus]) {
            const TxSlot& s = m_tx[bus][m_tx_tail[bus]];
            FDCAN_TxHeaderTypeDef hdr = {};
            hdr.Identifier = s.id;
            hdr.IdType = s.ext ? FDCAN_EXTENDED_ID : FDCAN_STANDARD_ID;
            hdr.TxFrameType = FDCAN_DATA_FRAME;
            hdr.DataLength = s.dlc;  /* classic: 1:1 encoding */
            hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
            hdr.BitRateSwitch = FDCAN_BRS_OFF;
            hdr.FDFormat = FDCAN_CLASSIC_CAN;
            hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
            if (HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr,
                    const_cast<uint8_t*>(s.data)) != HAL_OK) {
                break;
            }
            ++m_tx_frames[bus];
            m_tx_tail[bus] = (m_tx_tail[bus] + 1) % TX_RING;
            continue;
        }
        if (m_fd_tx_tail[bus] == m_fd_tx_head[bus]) break;
        const FdTxSlot& s = m_fd_tx[bus][m_fd_tx_tail[bus]];
        FDCAN_TxHeaderTypeDef hdr = {};
        hdr.Identifier = s.id;
        hdr.IdType = FDCAN_STANDARD_ID;
        hdr.TxFrameType = FDCAN_DATA_FRAME;
        hdr.DataLength = bytesToDlc(s.length);
        hdr.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
        hdr.BitRateSwitch = FDCAN_BRS_ON;
        hdr.FDFormat = FDCAN_FD_CAN;
        hdr.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
        if (HAL_FDCAN_AddMessageToTxFifoQ(h, &hdr,
                const_cast<uint8_t*>(s.data)) != HAL_OK) break;
        ++m_fd_tx_frames[bus];
        m_fd_tx_tail[bus] = (m_fd_tx_tail[bus] + 1U) % FD_TX_RING;
    }
}

void CanBus::recoverIfBusOff(uint8_t bus) {
    FDCAN_HandleTypeDef* h = handle(bus);
    if ((h->Instance->PSR & FDCAN_PSR_BO) == 0) {
        return;
    }
    /* Hardware auto-recovery is off (default): request init release and
     * restart the controller. */
    (void)HAL_FDCAN_Stop(h);
    if (HAL_FDCAN_Start(h) == HAL_OK) {
        ++m_busoff_recoveries[bus];
        Telemetry::printf("[CAN] bus %u: recovered from bus-off", bus);
    }
}

void CanBus::setRxHook(RxHook hook, void* user) {
    m_hook = hook;
    m_hook_user = user;
}

void CanBus::storeRx(uint8_t bus, const Frame& f) {
    for (size_t i = 0; i < RX_MAILBOXES; ++i) {
        if (m_mail[bus][i].used && m_mail[bus][i].id == f.id &&
            m_mail[bus][i].frame.ext == f.ext) {
            /* A generated high-rate domain may read this mailbox from an ISR.
             * Prevent it from observing a partially copied frame. */
            const uint32_t primask = irqSave();
            m_mail[bus][i].frame = f;
            ++m_mail[bus][i].seq;
            irqRestore(primask);
            return;
        }
    }
}

bool CanBus::rxLatest(uint8_t bus, uint32_t id, bool ext, Frame& out,
                      uint32_t* seqOut) {
    if (!enabled(bus) || !validId(id, ext)) {
        return false;
    }
    /* Find (or lazily subscribe) the mailbox for this ID. */
    const uint32_t primask = irqSave();
    size_t free_slot = RX_MAILBOXES;
    for (size_t i = 0; i < RX_MAILBOXES; ++i) {
        if (m_mail[bus][i].used && m_mail[bus][i].id == id &&
            m_mail[bus][i].frame.ext == ext) {
            out = m_mail[bus][i].frame;
            if (seqOut != nullptr) {
                *seqOut = m_mail[bus][i].seq;
            }
            const bool received = m_mail[bus][i].seq > 0;
            irqRestore(primask);
            return received;
        }
        if (!m_mail[bus][i].used && free_slot == RX_MAILBOXES) {
            free_slot = i;
        }
    }
    if (free_slot < RX_MAILBOXES) {
        m_mail[bus][free_slot].used = true;
        m_mail[bus][free_slot].id = id;
        m_mail[bus][free_slot].frame.ext = ext;
        m_mail[bus][free_slot].frame.id = id;
    }
    irqRestore(primask);
    return false;
}

void CanBus::processRx(uint8_t bus) {
    /* A full ring can be drained per update without allowing continuously
     * arriving traffic to starve the rest of the foreground loop. */
    for (size_t count = 0; count < RX_RING; ++count) {
        const size_t tail = m_rx_tail[bus];
        if (tail == m_rx_head[bus]) {
            break;
        }
        __DMB();
        const Frame f = s_rx_ring[bus][tail];
        __DMB();
        m_rx_tail[bus] = (tail + 1) % RX_RING;

        storeRx(bus, f);
        m_recent[bus][m_recent_head[bus]] = f;
        m_recent_head[bus] = (m_recent_head[bus] + 1) % RX_RECENT;

        if (m_hook != nullptr) {
            m_hook(bus, f, m_hook_user);
        }
    }
}

void CanBus::onRxFifo0(FDCAN_HandleTypeDef* h) {
    if (h == nullptr || (h->Instance != FDCAN1 && h->Instance != FDCAN2)) {
        return;
    }
    const uint8_t bus = (h->Instance == FDCAN1) ? 0 : 1;
    FDCAN_RxHeaderTypeDef hdr;
    while (HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) > 0) {
        Frame f = {};
        if (HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &hdr, f.data) != HAL_OK) {
            ++m_rx_hal_errors[bus];
            break;
        }
        ++m_rx_frames[bus];
        f.id = hdr.Identifier;
        f.ext = (hdr.IdType == FDCAN_EXTENDED_ID);
        f.dlc = dlcToBytes(hdr.DataLength);
        if (hdr.RxFrameType != FDCAN_DATA_FRAME || f.dlc > 8 ||
            !validId(f.id, f.ext)) {
            ++m_rx_malformed[bus];
            continue;
        }

        const size_t head = m_rx_head[bus];
        const size_t next = (head + 1) % RX_RING;
        if (next == m_rx_tail[bus]) {
            ++m_rx_queue_dropped[bus];
            continue;
        }
        s_rx_ring[bus][head] = f;
        __DMB();
        m_rx_head[bus] = next;
    }
}

void CanBus::printStatus(uint8_t bus) const {
    if (bus >= NUM_BUSES) {
        return;
    }
    FDCAN_HandleTypeDef* h = handle(bus);
    Telemetry::printf("[SHELL] can %s: en=%d fd=%d rate=%lu tx=%lu tx_drop=%lu fd_tx=%lu fd_drop=%lu rx=%lu rx_q_drop=%lu malformed=%lu hal_err=%lu busoff_rec=%lu PSR=0x%02lX",
                      bus == 0 ? "A(FDCAN1)" : "B(FDCAN2)",
                      m_enabled[bus] ? 1 : 0,
                      m_fd_enabled[bus] ? 1 : 0,
                      static_cast<unsigned long>(m_bitrate),
                      static_cast<unsigned long>(m_tx_frames[bus]),
                      static_cast<unsigned long>(m_tx_dropped[bus]),
                      static_cast<unsigned long>(m_fd_tx_frames[bus]),
                      static_cast<unsigned long>(m_fd_tx_dropped[bus]),
                      static_cast<unsigned long>(m_rx_frames[bus]),
                      static_cast<unsigned long>(m_rx_queue_dropped[bus]),
                      static_cast<unsigned long>(m_rx_malformed[bus]),
                      static_cast<unsigned long>(m_rx_hal_errors[bus]),
                      static_cast<unsigned long>(m_busoff_recoveries[bus]),
                      static_cast<unsigned long>(h->Instance->PSR));
}

void CanBus::printRecentRx(uint8_t bus) const {
    if (bus >= NUM_BUSES) {
        return;
    }
    for (size_t k = 0; k < RX_RECENT; ++k) {
        const Frame& f = m_recent[bus][(m_recent_head[bus] + k) % RX_RECENT];
        if (f.dlc == 0 && f.id == 0) {
            continue;
        }
        Telemetry::printf("[SHELL]   id=0x%03lX dlc=%u data=%02X %02X %02X %02X %02X %02X %02X %02X",
                          static_cast<unsigned long>(f.id), f.dlc,
                          f.data[0], f.data[1], f.data[2], f.data[3],
                          f.data[4], f.data[5], f.data[6], f.data[7]);
    }
}

} // namespace Inverter

/* FDCAN IT0 handlers (RX FIFO0 + error-status line).  CubeMX's it.c has no
 * FDCAN handlers; keep them here next to the driver. */
extern "C" void FDCAN1_IT0_IRQHandler(void) {
    HAL_FDCAN_IRQHandler(&hfdcan1);
}

extern "C" void FDCAN2_IT0_IRQHandler(void) {
    HAL_FDCAN_IRQHandler(&hfdcan2);
}

extern "C" void FDCAN1_IT1_IRQHandler(void) {
    HAL_FDCAN_IRQHandler(&hfdcan1);
    Inverter::canBus().serviceTx(&hfdcan1);
}

extern "C" void FDCAN2_IT1_IRQHandler(void) {
    HAL_FDCAN_IRQHandler(&hfdcan2);
    Inverter::canBus().serviceTx(&hfdcan2);
}

extern "C" void HAL_FDCAN_TxFifoEmptyCallback(FDCAN_HandleTypeDef* hfdcan) {
    Inverter::canBus().serviceTx(hfdcan);
}

extern "C" void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef* hfdcan,
                                          uint32_t RxFifo0ITs) {
    (void)RxFifo0ITs;
    Inverter::canBus().onRxFifo0(hfdcan);
}
