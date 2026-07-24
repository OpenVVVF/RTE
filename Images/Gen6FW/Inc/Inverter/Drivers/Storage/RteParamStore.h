#pragma once

#include "cy15b102q_driver.h"
#include "Inverter/Drivers/Storage/FramStore.h"

#include <cstdint>
#include <cstddef>

namespace Inverter {
namespace RteParamStore {

/**
 * @brief Named key-value store for RTE codegen parameters on F-RAM.
 *
 * Uses FramStore slots 2..N.  Slot 2 is a header; slots 3+ hold packed
 * entries of {key_len, flags, key[32], value, crc}.  The layout is kept
 * ASCII-debuggable: dumping any slot shows human-readable key names.
 *
 * The store is scanned at boot and cached in RAM.  All operations are
 * linear scans; with <= 64 entries this is fast enough for shell commands.
 */

constexpr uint32_t MAGIC          = 0x5254454BUL; /* 'RTEK' */
constexpr uint16_t NODE_HEADER    = 2U;
constexpr uint16_t NODE_FIRST_KV  = 3U;
constexpr uint16_t MAX_ENTRIES    = 64U;
constexpr uint16_t MAX_KEY_LEN    = 32U;
constexpr uint16_t ENTRY_SIZE     = 48U; /* 1+1+2+32+4+4+4 pad */
constexpr uint16_t ENTRIES_PER_SLOT = FramStore::SLOT_SIZE / ENTRY_SIZE;

struct Entry {
    uint8_t  key_len;
    uint8_t  flags;      /* 0 = valid, 1 = deleted */
    uint16_t reserved;
    char     key[MAX_KEY_LEN];
    float    value;
    uint32_t crc;
};
static_assert(sizeof(Entry) <= ENTRY_SIZE, "entry too large");

struct Header {
    uint32_t magic;
    uint32_t count;
    uint32_t reserved[2];
};
static_assert(sizeof(Header) == 16, "unexpected header size");

/** Bind to an initialised F-RAM device and scan the store into RAM. */
void init(CY15B102Q_HandleTypeDef* dev);

/** True if the store was initialised and scanned successfully. */
bool isReady();

/** Number of valid entries currently cached. */
size_t count();

/** Look up an entry by key.  Returns nullptr if not found. */
const Entry* find(const char* key);

/** Get a value by key.  Returns false if not found. */
bool get(const char* key, float* value);

/** Set a value by key, inserting if necessary.  Returns false on store full. */
bool set(const char* key, float value);

/** Delete a key (marks deleted, does not compact). */
bool remove(const char* key);

/** Clear all entries (rewrites header with count=0). */
bool clear();

/** Persist the RAM cache back to FRAM. */
bool flush();

/** Iterate all valid entries.  cb(key, value, user) — return false to stop. */
using IterateCallback = bool (*)(const char* key, float value, void* user);
void iterate(IterateCallback cb, void* user);

} // namespace RteParamStore
} // namespace Inverter
