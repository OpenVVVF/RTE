#include "Inverter/Drivers/CAN/CanBus.h"

#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include <cstring>

namespace Inverter {

namespace {
CanBus s_instance;

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
} // namespace

CanBus& canBus() {
    return s_instance;
}

FDCAN_HandleTypeDef* CanBus::handle(uint8_t bus) const {
    return (bus == 0) ? &hfdcan1 : &hfdcan2;
}

bool CanBus::enabled(uint8_t bus) const {
    return bus < NUM_BUSES && m_enabled[bus];
}

bool CanBus::applyTiming(uint8_t bus, uint32_t rate) {
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
    h->Init.NominalPrescaler = best_presc;
    h->Init.NominalSyncJumpWidth = 4;
    h->Init.NominalTimeSeg1 = 17;
    h->Init.NominalTimeSeg2 = 6;
    if (HAL_FDCAN_Init(h) != HAL_OK) {
        return false;
    }
    Telemetry::printf("[CAN] bus %u: %lu bit/s (presc %lu, actual %lu)",
                      static_cast<unsigned>(bus),
                      static_cast<unsigned long>(rate),
                      static_cast<unsigned long>(best_presc),
                      static_cast<unsigned long>(actual));
    return true;
}

bool CanBus::init() {
    m_enabled[0] = kvOr("Can.A.En", 0.0f) != 0.0f;
    m_enabled[1] = kvOr("Can.B.En", 1.0f) != 0.0f;
    m_bitrate = static_cast<uint32_t>(kvOr("Can.BitRate", 500000.0f));

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
        if (!applyTiming(bus, m_bitrate)) {
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
    }

    /* RX FIFO0 + error interrupts share IT0 on each peripheral. */
    HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 6, 0);
    HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
    HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);

    return m_enabled[0] || m_enabled[1];
}

bool CanBus::send(uint8_t bus, uint32_t id, bool ext, const uint8_t* data,
                  uint8_t dlc) {
    if (!enabled(bus) || dlc > 8 || data == nullptr) {
        return false;
    }
    __disable_irq();
    size_t next = (m_tx_head[bus] + 1) % TX_RING;
    if (next == m_tx_tail[bus]) {
        /* Full: drop the oldest. */
        m_tx_tail[bus] = (m_tx_tail[bus] + 1) % TX_RING;
        ++m_tx_dropped[bus];
    }
    TxSlot& s = m_tx[bus][m_tx_head[bus]];
    s.id = id;
    s.ext = ext;
    s.dlc = dlc;
    std::memcpy(s.data, data, dlc);
    m_tx_head[bus] = next;
    __enable_irq();
    return true;
}

void CanBus::update() {
    for (uint8_t bus = 0; bus < NUM_BUSES; ++bus) {
        if (!enabled(bus)) {
            continue;
        }
        recoverIfBusOff(bus);

        FDCAN_HandleTypeDef* h = handle(bus);
        while (m_tx_tail[bus] != m_tx_head[bus] &&
               HAL_FDCAN_GetTxFifoFreeLevel(h) > 0) {
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
                break;  /* FIFO state race; retry next update */
            }
            ++m_tx_frames[bus];
            m_tx_tail[bus] = (m_tx_tail[bus] + 1) % TX_RING;
        }
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
            m_mail[bus][i].frame = f;
            ++m_mail[bus][i].seq;
            return;
        }
    }
}

bool CanBus::rxLatest(uint8_t bus, uint32_t id, Frame& out, uint32_t* seqOut) {
    if (!enabled(bus)) {
        return false;
    }
    /* Find (or lazily subscribe) the mailbox for this ID. */
    size_t free_slot = RX_MAILBOXES;
    for (size_t i = 0; i < RX_MAILBOXES; ++i) {
        if (m_mail[bus][i].used && m_mail[bus][i].id == id) {
            out = m_mail[bus][i].frame;
            if (seqOut != nullptr) {
                *seqOut = m_mail[bus][i].seq;
            }
            return m_mail[bus][i].seq > 0;
        }
        if (!m_mail[bus][i].used && free_slot == RX_MAILBOXES) {
            free_slot = i;
        }
    }
    if (free_slot < RX_MAILBOXES) {
        m_mail[bus][free_slot].used = true;
        m_mail[bus][free_slot].id = id;
        m_mail[bus][free_slot].frame.ext = false;
        m_mail[bus][free_slot].frame.id = id;
    }
    return false;
}

void CanBus::onRxFifo0(FDCAN_HandleTypeDef* h) {
    const uint8_t bus = (h->Instance == FDCAN1) ? 0 : 1;
    FDCAN_RxHeaderTypeDef hdr;
    while (HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) > 0) {
        Frame f = {};
        if (HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &hdr, f.data) != HAL_OK) {
            break;
        }
        f.id = hdr.Identifier;
        f.ext = (hdr.IdType == FDCAN_EXTENDED_ID);
        f.dlc = dlcToBytes(hdr.DataLength);
        ++m_rx_frames[bus];

        storeRx(bus, f);

        m_recent[bus][m_recent_head[bus]] = f;
        m_recent_head[bus] = (m_recent_head[bus] + 1) % RX_RECENT;

        if (m_hook != nullptr) {
            m_hook(bus, f, m_hook_user);
        }
    }
}

void CanBus::printStatus(uint8_t bus) const {
    if (bus >= NUM_BUSES) {
        return;
    }
    FDCAN_HandleTypeDef* h = handle(bus);
    Telemetry::printf("[SHELL] can %s: en=%d rate=%lu tx=%lu drop=%lu rx=%lu busoff_rec=%lu PSR=0x%02lX",
                      bus == 0 ? "A(FDCAN1)" : "B(FDCAN2)",
                      m_enabled[bus] ? 1 : 0,
                      static_cast<unsigned long>(m_bitrate),
                      static_cast<unsigned long>(m_tx_frames[bus]),
                      static_cast<unsigned long>(m_tx_dropped[bus]),
                      static_cast<unsigned long>(m_rx_frames[bus]),
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

extern "C" void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef* hfdcan,
                                          uint32_t RxFifo0ITs) {
    (void)RxFifo0ITs;
    Inverter::canBus().onRxFifo0(hfdcan);
}
