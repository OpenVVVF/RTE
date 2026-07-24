#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Drivers/Storage/FramStore.h"

#include <cstring>

namespace Inverter {
namespace RteParamStore {

namespace {

CY15B102Q_HandleTypeDef* s_dev = nullptr;
Entry s_cache[MAX_ENTRIES];
size_t s_count = 0;
bool s_ready = false;

uint32_t entryCrc(const Entry& e) {
    uint32_t crc = 0xFFFFFFFFUL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&e);
    for (size_t i = 0; i < offsetof(Entry, crc); ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

bool validEntry(const Entry& e) {
    if (e.key_len == 0 || e.key_len > MAX_KEY_LEN) return false;
    if (e.flags != 0) return false;
    return entryCrc(e) == e.crc;
}

uint32_t slotForEntry(size_t idx) {
    return FramStore::slotAddress(NODE_FIRST_KV + idx / ENTRIES_PER_SLOT);
}

size_t offsetInSlot(size_t idx) {
    return (idx % ENTRIES_PER_SLOT) * ENTRY_SIZE;
}

bool readEntry(size_t idx, Entry& e) {
    uint8_t buf[ENTRY_SIZE];
    CY15B102Q_Read(s_dev, slotForEntry(idx) + offsetInSlot(idx), buf, ENTRY_SIZE);
    std::memcpy(&e, buf, sizeof(e));
    return validEntry(e);
}

bool writeEntry(size_t idx, const Entry& e) {
    uint8_t buf[ENTRY_SIZE] = {};
    std::memcpy(buf, &e, sizeof(e));
    CY15B102Q_Write(s_dev, slotForEntry(idx) + offsetInSlot(idx), buf, ENTRY_SIZE);
    return true;
}

bool readHeader(Header& h) {
    CY15B102Q_Read(s_dev, FramStore::slotAddress(NODE_HEADER),
                   reinterpret_cast<uint8_t*>(&h), sizeof(h));
    return h.magic == MAGIC;
}

bool writeHeader() {
    Header h = {MAGIC, static_cast<uint32_t>(s_count), {0, 0}};
    CY15B102Q_Write(s_dev, FramStore::slotAddress(NODE_HEADER),
                    reinterpret_cast<const uint8_t*>(&h), sizeof(h));
    return true;
}

} // namespace

void init(CY15B102Q_HandleTypeDef* dev) {
    s_dev = dev;
    s_count = 0;
    s_ready = false;

    if (s_dev == nullptr) return;

    Header h;
    if (!readHeader(h)) {
        /* No valid header: start empty. */
        writeHeader();
        s_ready = true;
        return;
    }

    s_count = (h.count < MAX_ENTRIES) ? h.count : MAX_ENTRIES;
    size_t loaded = 0;
    for (size_t i = 0; i < s_count && loaded < MAX_ENTRIES; ++i) {
        Entry e;
        if (readEntry(i, e)) {
            s_cache[loaded++] = e;
        }
    }
    s_count = loaded;
    s_ready = true;
}

bool isReady() {
    return s_ready;
}

size_t count() {
    return s_count;
}

const Entry* find(const char* key) {
    if (!s_ready || key == nullptr) return nullptr;
    for (size_t i = 0; i < s_count; ++i) {
        if (std::strncmp(s_cache[i].key, key, MAX_KEY_LEN) == 0) {
            return &s_cache[i];
        }
    }
    return nullptr;
}

bool get(const char* key, float* value) {
    const Entry* e = find(key);
    if (e == nullptr || value == nullptr) return false;
    *value = e->value;
    return true;
}

bool set(const char* key, float value) {
    if (!s_ready || key == nullptr) return false;
    const size_t key_len = std::strlen(key);
    if (key_len == 0 || key_len >= MAX_KEY_LEN) return false;

    /* Update existing. */
    for (size_t i = 0; i < s_count; ++i) {
        if (std::strncmp(s_cache[i].key, key, MAX_KEY_LEN) == 0) {
            s_cache[i].value = value;
            s_cache[i].crc = entryCrc(s_cache[i]);
            return true;
        }
    }

    /* Insert new. */
    if (s_count >= MAX_ENTRIES) return false;
    Entry& e = s_cache[s_count];
    e.key_len = static_cast<uint8_t>(key_len);
    e.flags = 0;
    e.reserved = 0;
    std::strncpy(e.key, key, MAX_KEY_LEN - 1);
    e.key[MAX_KEY_LEN - 1] = '\0';
    e.value = value;
    e.crc = entryCrc(e);
    ++s_count;
    return true;
}

bool remove(const char* key) {
    if (!s_ready || key == nullptr) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (std::strncmp(s_cache[i].key, key, MAX_KEY_LEN) == 0) {
            /* Shift remaining entries down. */
            for (size_t j = i; j + 1 < s_count; ++j) {
                s_cache[j] = s_cache[j + 1];
            }
            --s_count;
            return true;
        }
    }
    return false;
}

bool clear() {
    if (!s_ready) return false;
    s_count = 0;
    return writeHeader();
}

bool flush() {
    if (!s_ready) return false;
    if (!writeHeader()) return false;
    for (size_t i = 0; i < s_count; ++i) {
        if (!writeEntry(i, s_cache[i])) return false;
    }
    /* Clear any stale entries beyond the current count. */
    Entry blank = {};
    for (size_t i = s_count; i < MAX_ENTRIES; ++i) {
        writeEntry(i, blank);
    }
    return true;
}

void iterate(IterateCallback cb, void* user) {
    if (!s_ready || cb == nullptr) return;
    for (size_t i = 0; i < s_count; ++i) {
        if (!cb(s_cache[i].key, s_cache[i].value, user)) {
            break;
        }
    }
}

} // namespace RteParamStore
} // namespace Inverter
