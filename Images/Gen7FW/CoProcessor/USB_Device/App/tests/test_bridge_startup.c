#include "../bridge_app_ready.h"
#include "../bridge_line.h"

#include <assert.h>
#include <string.h>

static size_t frame(uint8_t *out, bool corrupt, uint8_t type)
{
    uint8_t raw[IVP_HEADER_SIZE + 3u] = {0};
    ivp_write_u32le(raw, IVP_MAGIC);
    raw[4] = IVP_VERSION;
    raw[5] = type;
    ivp_write_u16le(raw + 6, 1u);
    ivp_write_u16le(raw + sizeof(raw) - 2u,
                    ivp_crc16_ccitt(raw, sizeof(raw) - 2u));
    if (corrupt) raw[8] ^= 1u;
    size_t length = ivp_cobs_encode(raw, sizeof(raw), out, 128u);
    out[length++] = 0u;
    return length;
}
static bool feed(BridgeAppReady *ready, const uint8_t *bytes, size_t length,
                  uint32_t now_ms)
{
    bool valid = false;
    for (size_t i = 0; i < length; ++i)
        valid |= BridgeAppReady_Feed(ready, bytes[i], now_ms);
    return valid;
}
static void readiness(void)
{
    BridgeAppReady ready;
    uint8_t bytes[128];
    BridgeAppReady_Begin(&ready, 0u, false);
    const uint8_t noise[] = {0u, 0u, 0xFFu, 0x01u, 0u};
    assert(!feed(&ready, noise, sizeof(noise), 2000u));
    /* Time alone never authorizes a command, even after the old 1.5 s wait. */
    assert(!BridgeAppReady_Feed(&ready, 0u, 60000u));
    size_t length = frame(bytes, true, IVP_MSG_TELEMETRY_DATA);
    assert(!feed(&ready, bytes, length, 60001u));
    length = frame(bytes, false, IVP_MSG_COMMAND_RSP);
    assert(!feed(&ready, bytes, length, 60002u));
    length = frame(bytes, false, IVP_MSG_TELEMETRY_DEFINE);
    assert(!feed(&ready, bytes, length - 1u, 60003u));
    assert(BridgeAppReady_Feed(&ready, 0u, 60004u));

    BridgeAppReady_Begin(&ready, 100u, true);
    /* A shell reboot must not reopen on old DMA telemetry still in flight. */
    assert(!feed(&ready, bytes, length, 101u));
    assert(!feed(&ready, bytes, length, 119u));
    assert(feed(&ready, bytes, length, 139u));

    BridgeAppReady_Begin(&ready, UINT32_MAX - 10u, true);
    assert(feed(&ready, bytes, length, 10u)); /* Millisecond rollover. */

    BridgeAppReady_Begin(&ready, 0u, false);
    for (unsigned i = 0; i < BRIDGE_APP_ENCODED_MAX + 5u; ++i)
        assert(!BridgeAppReady_Feed(&ready, 1u, 0u));
    assert(!BridgeAppReady_Feed(&ready, 0u, 0u));
    assert(feed(&ready, bytes, length, 1u));
}
static void complete_lines(void)
{
    uint8_t ring[256] = {0};
    /* USB split and every DMA wrap offset: no prefix is forwarded until the
     * terminating byte is queued. Once queued, all wrap chunks can drain. */
    const uint8_t command[] = "\x18\nprobe\n";
    for (unsigned tail = 0; tail < sizeof(ring); ++tail) {
        for (unsigned n = 0; n < sizeof(command) - 1u; ++n)
            ring[(tail + n) % sizeof(ring)] = command[n];
        BridgeLineChunk chunk = BridgeLine_Next(ring, sizeof(ring),
                                                (tail + 1u) % sizeof(ring), tail, false);
        assert(chunk.length == 0u); /* CAN without its newline stays staged. */
        uint16_t next = tail;
        const uint16_t head = (tail + sizeof(command) - 1u) % sizeof(ring);
        unsigned sent = 0u;
        while (next != head) {
            chunk = BridgeLine_Next(ring, sizeof(ring), head, next, false);
            assert(chunk.length != 0u && chunk.dropped == 0u);
            next = (chunk.tail + chunk.length) % sizeof(ring);
            sent += chunk.length;
        }
        assert(sent == sizeof(command) - 1u);
    }

    memset(ring, 'x', sizeof(ring));
    BridgeLineChunk chunk = BridgeLine_Next(ring, sizeof(ring), 200u, 0u, false);
    assert(chunk.length == 0u && chunk.discard && chunk.dropped == 200u);
    ring[200] = BRIDGE_SHELL_CANCEL;
    ring[201] = '\n';
    chunk = BridgeLine_Next(ring, sizeof(ring), 202u, chunk.tail, chunk.discard);
    assert(chunk.length == 2u && !chunk.discard && chunk.dropped == 0u);

    memset(ring, 'x', sizeof(ring));
    ring[127] = '\n';
    chunk = BridgeLine_Next(ring, sizeof(ring), 128u, 0u, false);
    assert(chunk.length == 128u); /* Maximum H7 line, including its newline. */
}
int main(void)
{
    readiness();
    complete_lines();
    return 0;
}
