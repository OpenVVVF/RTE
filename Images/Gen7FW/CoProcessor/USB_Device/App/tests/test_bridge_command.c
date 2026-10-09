#include "../bridge_command.h"
#include "../bridge_dma_math.h"

#include <assert.h>
#include <string.h>

static void check_line(const uint8_t *line, uint16_t length, bool expected)
{
    /* A USB packet or the circular TX ring can split a command anywhere. */
    for (uint16_t split = 0u; split < length; ++split) {
        BridgeCommand command = {0};
        assert(!BridgeCommand_Feed(&command, line, split));
        assert(BridgeCommand_Feed(&command, line + split, length - split) == expected);
        assert(command.length == 0u);
    }
}

static void check_text(const char *line, bool expected)
{
    check_line((const uint8_t *)line, (uint16_t)strlen(line), expected);
}

int main(void)
{
    check_text("reboot\n", true);
    check_text(" \tReBoOt \t\v\f\r", true);
    check_text("reboot extra\n", false);
    check_text("reboots\n", false);
    check_text("re boo t\n", false);
    check_text("boot\n", false);
    check_text("help reboot\n", false);
    check_text("\n", false);
    check_text(" \t\n", false);

    const uint8_t embedded_nul[] = "reboot\0ignored\n";
    check_line(embedded_nul, sizeof(embedded_nul) - 1u, true);
    const uint8_t leading_nul[] = "\0reboot\n";
    check_line(leading_nul, sizeof(leading_nul) - 1u, false);

    uint8_t long_line[130];
    memset(long_line, ' ', sizeof(long_line));
    memcpy(long_line, "reboot", 6u);
    long_line[127] = '\n'; /* 127 bytes fit in the H7 line buffer. */
    check_line(long_line, 128u, true);
    long_line[127] = ' ';
    long_line[128] = '\n';
    check_line(long_line, 129u, false);

    /* A held terminator must be recognized again without consuming parser
     * state; unrelated commands and CRLF cannot authorize a second reboot. */
    BridgeCommand command = {0};
    assert(!BridgeCommand_Feed(&command, (const uint8_t *)"reb", 3u));
    BridgeCommand pending = command;
    assert(BridgeCommand_Feed(&pending, (const uint8_t *)"oot\r", 4u));
    assert(command.matched == 3u);
    assert(BridgeCommand_Feed(&command, (const uint8_t *)"oot\r", 4u));
    assert(!BridgeCommand_Feed(&command, (const uint8_t *)"\n", 1u));
    assert(!BridgeCommand_Feed(&command, (const uint8_t *)"status\n", 7u));

    const uint8_t batched[] = "help\nreboot\nraw\n";
    unsigned resets = 0u;
    for (uint16_t offset = 0u; offset < sizeof(batched) - 1u;) {
        const uint16_t count = BridgeDma_TxChunk(batched + offset,
                                                sizeof(batched) - 1u - offset, 0u);
        if (BridgeCommand_Feed(&command, batched + offset, count)) ++resets;
        offset += count;
    }
    assert(resets == 1u);
    return 0;
}
