#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hostsim {

/* Multi-instance CAN bridge: shares this process' platform_can_send traffic
 * with other concurrently running host_sim instances over localhost TCP
 * (Linux only; on Windows the flags print a notice and the bridge stays off).
 *
 * Topology is hub-and-spoke: one instance listens (--can-bridge-listen PORT),
 * the others connect (--can-bridge-connect HOST:PORT). The hub rebroadcasts
 * every record it receives to all other connected spokes, so every
 * participant sees everyone else's frames.
 *
 * Wire format v1 (all little-endian):
 *   u16 payload_len (= 24)
 *   u32 magic 0x314E4143 ("CAN1")
 *   u32 src instance id
 *   u32 CAN id
 *   u8  bus / u8 ext / u8 dlc / u8 reserved
 *   u8  data[8]
 *
 * Robustness contract: everything is non-blocking; the sim loop never waits
 * on the bridge. A failed connect, a dead peer or a lost hub is logged once
 * and the sim continues unbridged (no reconnect in v1). A peer that cannot
 * keep up simply misses frames (counted, not queued).
 */
struct CanBridgeConfig {
    bool hub = false;                 /* --can-bridge-listen PORT          */
    bool connect = false;             /* --can-bridge-connect HOST:PORT    */
    std::string host = "127.0.0.1";   /* hub address (connect mode)        */
    int port = 0;
    /* Frame origin tag; a received record tagged with our own id is treated
     * as looped back and dropped. 0 = derive from the pid (unique among
     * concurrent instances; the default telemetry port is not). */
    uint32_t instance_id = 0;
    bool debug = false;               /* --can-bridge-debug: also log tx   */
    bool selftest = false;            /* --can-selftest                    */
};

class CanBridge {
public:
    void Configure(const CanBridgeConfig& cfg);
    bool Enabled() const { return enabled_; }

    /* Open the hub listen socket / start the non-blocking connect and print
     * the one-line startup announcement. */
    bool Start();
    /* Accept spokes, drain reads, inject received frames and run the
     * selftest emitter. Called once per app-loop tick with sim time. */
    void Poll(float now_s);
    /* Mirror one platform_can_send onto the bridge. No-op when unconfigured;
     * never blocks. */
    void Publish(uint8_t bus, uint32_t id, bool ext, const uint8_t* data,
                 uint8_t dlc);
    void Shutdown();

private:
    struct Peer {
        int fd = -1;
        uint8_t rx[64] = {0};
        size_t rx_len = 0;
    };

    bool StartHub();
    bool StartSpoke();
    void AcceptAll();
    void CheckConnect();
    void ServicePeer(Peer& p, Peer* rebroadcast_origin);
    bool HandleRecord(const uint8_t* record, Peer* rebroadcast_origin);
    /* true = record written; on a hard error the peer fd is closed (-1). */
    bool SendRecord(Peer& p, const uint8_t* record, size_t len);
    void DropHubLink(const char* reason);
    void RunSelftest(float now_s);

    CanBridgeConfig cfg_{};
    bool configured_ = false;
    bool enabled_ = false;
    bool is_hub_ = false;

    int listen_fd_ = -1;
    std::vector<Peer> peers_;         /* hub: accepted spokes              */

    enum class SpokeState { Idle, Pending, Connected, Down };
    SpokeState spoke_state_ = SpokeState::Idle;
    Peer hub_link_{};                 /* spoke: the single hub connection  */
    uint64_t connect_start_ms_ = 0;

    uint64_t tx_frames_ = 0;
    uint64_t rx_frames_ = 0;
    uint64_t dropped_ = 0;            /* send would-block / partial writes */
    uint64_t filtered_ = 0;           /* received records with our own id  */

    float next_selftest_s_ = 0.0f;
    uint32_t selftest_step_ = 0;
};

CanBridge& GlobalCanBridge();

} // namespace hostsim
