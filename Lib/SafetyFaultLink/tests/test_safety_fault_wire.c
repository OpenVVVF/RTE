#include "safety_fault_wire.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    const SafetyFaultFrame sent = {
        .faults = 0x80000021u,
        .generation = 0xBEEF,
        .state = 3u,
        .clear_result = SAFETY_CLEAR_REFUSED
    };
    char line[SAFETY_FAULT_WIRE_LENGTH];
    SafetyFaultWire_Encode(&sent, line);
    assert(line[20] == '\n');
    SafetyFaultFrame received = {0};
    assert(SafetyFaultWire_Decode(line, 20u, &received));
    assert(received.faults == sent.faults);
    assert(received.generation == sent.generation);
    assert(received.state == sent.state);
    assert(received.clear_result == sent.clear_result);
    line[10] = line[10] == '0' ? '1' : '0';
    assert(!SafetyFaultWire_Decode(line, 20u, &received));
    assert(!SafetyFaultWire_Decode(line, 19u, &received));
    assert(!SafetyFaultWire_Decode("!SF1000000000000000000", 20u, NULL));
    return 0;
}
