/* CAN receive mailbox for Id on Bus (1=A/FDCAN1, 2=B/FDCAN2).
 * Fresh is true for one step when a new frame arrived; Dlc/bytes hold the
 * latest payload otherwise. */
uint8_t buf[8] = {};
uint32_t seq = 0;
const int dlc = platform_can_rx(static_cast<uint8_t>(Bus),
                                static_cast<uint32_t>(Id), buf, &seq);
Fresh = (seq != LastSeq);
LastSeq = static_cast<float>(seq);
if (dlc > 0) {
    Dlc = static_cast<float>(dlc);
    D0 = buf[0]; D1 = buf[1]; D2 = buf[2]; D3 = buf[3];
    D4 = buf[4]; D5 = buf[5]; D6 = buf[6]; D7 = buf[7];
}
