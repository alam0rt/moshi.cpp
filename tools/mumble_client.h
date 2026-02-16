#pragma once
//
// mumble_client.h – Minimal Mumble client for moshi-mumble.
//
// TCP-tunnelled audio only (no raw UDP / OCB-AES128).
// Uses OpenSSL for TLS, protobuf for control messages, libopus for voice.
//

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <opus/opus.h>

#include "Mumble.pb.h"

// ---------------------------------------------------------------------------
// Mumble TCP message types (matches Mumble.proto order)
// ---------------------------------------------------------------------------
enum MumbleMessageType : uint16_t {
    MSG_Version         = 0,
    MSG_UDPTunnel       = 1,
    MSG_Authenticate    = 2,
    MSG_Ping            = 3,
    MSG_Reject          = 4,
    MSG_ServerSync      = 5,
    MSG_ChannelRemove   = 6,
    MSG_ChannelState    = 7,
    MSG_UserRemove      = 8,
    MSG_UserState       = 9,
    MSG_BanList         = 10,
    MSG_TextMessage      = 11,
    MSG_PermissionDenied = 12,
    MSG_ACL             = 13,
    MSG_QueryUsers      = 14,
    MSG_CryptSetup      = 15,
    MSG_ContextActionModify = 16,
    MSG_ContextAction   = 17,
    MSG_UserList         = 18,
    MSG_VoiceTarget     = 19,
    MSG_PermissionQuery = 20,
    MSG_CodecVersion    = 21,
    MSG_UserStats        = 22,
    MSG_RequestBlob     = 23,
    MSG_ServerConfig    = 24,
    MSG_SuggestConfig   = 25,
};

// ---------------------------------------------------------------------------
// Varint helpers (Mumble PacketDataStream encoding)
// ---------------------------------------------------------------------------
static inline int mumble_varint_encode(uint8_t *buf, uint64_t val) {
    if (val < 0x80) {
        buf[0] = (uint8_t)val;
        return 1;
    } else if (val < 0x4000) {
        buf[0] = (uint8_t)((val >> 8) | 0x80);
        buf[1] = (uint8_t)(val & 0xFF);
        return 2;
    } else if (val < 0x200000) {
        buf[0] = (uint8_t)((val >> 16) | 0xC0);
        buf[1] = (uint8_t)((val >> 8) & 0xFF);
        buf[2] = (uint8_t)(val & 0xFF);
        return 3;
    } else if (val < 0x10000000) {
        buf[0] = (uint8_t)((val >> 24) | 0xE0);
        buf[1] = (uint8_t)((val >> 16) & 0xFF);
        buf[2] = (uint8_t)((val >> 8) & 0xFF);
        buf[3] = (uint8_t)(val & 0xFF);
        return 4;
    } else {
        buf[0] = 0xF0;
        buf[1] = (uint8_t)((val >> 24) & 0xFF);
        buf[2] = (uint8_t)((val >> 16) & 0xFF);
        buf[3] = (uint8_t)((val >> 8) & 0xFF);
        buf[4] = (uint8_t)(val & 0xFF);
        return 5;
    }
}

static inline int mumble_varint_decode(const uint8_t *buf, int len, uint64_t &val) {
    if (len < 1) return -1;
    uint8_t h = buf[0];
    if ((h & 0x80) == 0) {
        val = h & 0x7F;
        return 1;
    } else if ((h & 0xC0) == 0x80) {
        if (len < 2) return -1;
        val = ((uint64_t)(h & 0x3F) << 8) | buf[1];
        return 2;
    } else if ((h & 0xE0) == 0xC0) {
        if (len < 3) return -1;
        val = ((uint64_t)(h & 0x1F) << 16) | ((uint64_t)buf[1] << 8) | buf[2];
        return 3;
    } else if ((h & 0xF0) == 0xE0) {
        if (len < 4) return -1;
        val = ((uint64_t)(h & 0x0F) << 24) | ((uint64_t)buf[1] << 16) |
              ((uint64_t)buf[2] << 8) | buf[3];
        return 4;
    } else if ((h & 0xFC) == 0xF0) {
        if (len < 5) return -1;
        val = ((uint64_t)buf[1] << 24) | ((uint64_t)buf[2] << 16) |
              ((uint64_t)buf[3] << 8) | buf[4];
        return 5;
    } else if ((h & 0xFC) == 0xF4) {
        // 64-bit
        if (len < 9) return -1;
        val = 0;
        for (int i = 1; i <= 8; i++)
            val = (val << 8) | buf[i];
        return 9;
    } else if ((h & 0xFC) == 0xF8) {
        // Negative recursive varint (not used for audio)
        val = 0;
        return 1;
    } else {
        // ~varint (complement)
        val = ~(uint64_t)(h & 0x03);
        return 1;
    }
}

// ---------------------------------------------------------------------------
// MumbleClient
// ---------------------------------------------------------------------------
class MumbleClient {
public:
    // Audio parameters matching the Moshi pipeline
    static constexpr int MOSHI_SAMPLE_RATE = 24000;
    static constexpr int MOSHI_FRAME_SIZE  = 1920;  // 80 ms

    // Opus parameters — 4 × 20 ms Opus frames = 1 Moshi frame
    static constexpr int OPUS_SAMPLE_RATE  = 24000;
    static constexpr int OPUS_FRAME_MS     = 20;
    static constexpr int OPUS_FRAME_SIZE   = OPUS_SAMPLE_RATE * OPUS_FRAME_MS / 1000; // 480
    static constexpr int OPUS_FRAMES_PER_MOSHI = MOSHI_FRAME_SIZE / OPUS_FRAME_SIZE;  // 4
    static constexpr int OPUS_BITRATE      = 64000;
    static constexpr int OPUS_MAX_PACKET   = 1024;

    MumbleClient() = default;
    ~MumbleClient() { disconnect(); }

    // -----------------------------------------------------------------------
    // Connection
    // -----------------------------------------------------------------------
    bool connect(const std::string &host, int port,
                 const std::string &username,
                 const std::string &password = "",
                 const std::string &channel = "")
    {
        host_     = host;
        port_     = port;
        username_ = username;
        password_ = password;
        channel_  = channel;

        // --- TCP socket ---
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0) {
            fprintf(stderr, "mumble: getaddrinfo failed for %s:%d\n", host.c_str(), port);
            return false;
        }

        sock_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock_fd_ < 0) {
            freeaddrinfo(res);
            fprintf(stderr, "mumble: socket() failed\n");
            return false;
        }

        if (::connect(sock_fd_, res->ai_addr, res->ai_addrlen) != 0) {
            freeaddrinfo(res);
            ::close(sock_fd_);
            sock_fd_ = -1;
            fprintf(stderr, "mumble: connect() failed to %s:%d\n", host.c_str(), port);
            return false;
        }
        freeaddrinfo(res);

        // --- TLS ---
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            fprintf(stderr, "mumble: SSL_CTX_new failed\n");
            return false;
        }
        // Mumble servers often use self-signed certs
        SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);

        ssl_ = SSL_new(ssl_ctx_);
        SSL_set_fd(ssl_, sock_fd_);
        if (SSL_connect(ssl_) != 1) {
            fprintf(stderr, "mumble: TLS handshake failed\n");
            ERR_print_errors_fp(stderr);
            return false;
        }

        printf("mumble: TLS connected to %s:%d\n", host.c_str(), port);

        // --- Opus codec ---
        int err;
        opus_enc_ = opus_encoder_create(OPUS_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK) {
            fprintf(stderr, "mumble: opus_encoder_create failed: %s\n", opus_strerror(err));
            return false;
        }
        opus_encoder_ctl(opus_enc_, OPUS_SET_BITRATE(OPUS_BITRATE));
        opus_encoder_ctl(opus_enc_, OPUS_SET_VBR(1));
        opus_encoder_ctl(opus_enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

        opus_dec_ = opus_decoder_create(OPUS_SAMPLE_RATE, 1, &err);
        if (err != OPUS_OK) {
            fprintf(stderr, "mumble: opus_decoder_create failed: %s\n", opus_strerror(err));
            return false;
        }

        // --- Mumble handshake ---
        if (!send_version()) return false;
        if (!send_authenticate()) return false;

        // Start the control/receive thread
        connected_ = true;
        recv_thread_ = std::thread(&MumbleClient::recv_loop, this);

        // Wait for ServerSync
        {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            std::unique_lock<std::mutex> lock(sync_mutex_);
            sync_cv_.wait_until(lock, deadline, [&]{ return synced_.load(); });
        }
        if (!synced_) {
            fprintf(stderr, "mumble: did not receive ServerSync within 10s\n");
            disconnect();
            return false;
        }

        printf("mumble: authenticated as \"%s\" (session %u)\n",
               username_.c_str(), session_id_);

        // Join requested channel if specified
        if (!channel_.empty()) {
            join_channel(channel_);
        }

        // Start the ping thread
        ping_thread_ = std::thread(&MumbleClient::ping_loop, this);

        return true;
    }

    void disconnect() {
        if (!connected_) return;
        connected_ = false;

        if (ping_thread_.joinable()) ping_thread_.join();
        if (recv_thread_.joinable()) recv_thread_.join();

        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ssl_ctx_) { SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
        if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }

        if (opus_enc_) { opus_encoder_destroy(opus_enc_); opus_enc_ = nullptr; }
        if (opus_dec_) { opus_decoder_destroy(opus_dec_); opus_dec_ = nullptr; }
    }

    bool is_connected() const { return connected_ && synced_; }

    // -----------------------------------------------------------------------
    // Audio RX — returns true when a full 1920-sample Moshi frame is ready
    // -----------------------------------------------------------------------
    bool receive_audio(float *frame_out) {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        if (rx_pcm_buf_.size() < (size_t)MOSHI_FRAME_SIZE) {
            // Not enough buffered — fill with silence
            memset(frame_out, 0, MOSHI_FRAME_SIZE * sizeof(float));
            return false;
        }
        std::copy(rx_pcm_buf_.begin(),
                  rx_pcm_buf_.begin() + MOSHI_FRAME_SIZE, frame_out);
        rx_pcm_buf_.erase(rx_pcm_buf_.begin(),
                          rx_pcm_buf_.begin() + MOSHI_FRAME_SIZE);
        return true;
    }

    // -----------------------------------------------------------------------
    // Audio TX — send a 1920-sample Moshi frame as 4 × 20 ms Opus packets
    // -----------------------------------------------------------------------
    void send_audio(const float *frame_in) {
        for (int i = 0; i < OPUS_FRAMES_PER_MOSHI; i++) {
            const float *sub = frame_in + i * OPUS_FRAME_SIZE;

            uint8_t opus_data[OPUS_MAX_PACKET];
            int opus_len = opus_encode_float(opus_enc_, sub, OPUS_FRAME_SIZE,
                                             opus_data, OPUS_MAX_PACKET);
            if (opus_len < 0) {
                fprintf(stderr, "mumble: opus_encode_float error: %s\n",
                        opus_strerror(opus_len));
                continue;
            }

            send_opus_packet(opus_data, opus_len,
                             i == (OPUS_FRAMES_PER_MOSHI - 1));
        }
    }

private:
    // -----------------------------------------------------------------------
    // TCP framing: 2-byte big-endian type + 4-byte big-endian length + payload
    // -----------------------------------------------------------------------
    bool tcp_send(uint16_t type, const uint8_t *data, uint32_t len) {
        std::lock_guard<std::mutex> lock(tx_mutex_);

        uint8_t header[6];
        header[0] = (uint8_t)(type >> 8);
        header[1] = (uint8_t)(type & 0xFF);
        header[2] = (uint8_t)(len >> 24);
        header[3] = (uint8_t)((len >> 16) & 0xFF);
        header[4] = (uint8_t)((len >> 8) & 0xFF);
        header[5] = (uint8_t)(len & 0xFF);

        if (SSL_write(ssl_, header, 6) != 6) return false;
        if (len > 0 && SSL_write(ssl_, data, (int)len) != (int)len) return false;
        return true;
    }

    bool tcp_send_proto(uint16_t type, const google::protobuf::MessageLite &msg) {
        std::string buf;
        msg.SerializeToString(&buf);
        return tcp_send(type, (const uint8_t *)buf.data(), (uint32_t)buf.size());
    }

    // Read exactly n bytes from TLS
    bool ssl_read_exact(uint8_t *buf, int n) {
        int got = 0;
        while (got < n) {
            int r = SSL_read(ssl_, buf + got, n - got);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Handshake
    // -----------------------------------------------------------------------
    bool send_version() {
        MumbleProto::Version msg;
        // Mumble version 1.5.0 encoded in v2 format:
        // major << 48 | minor << 32 | patch << 16
        msg.set_version_v2(((uint64_t)1 << 48) | ((uint64_t)5 << 32));
        msg.set_version_v1((1 << 16) | (5 << 8));
        msg.set_release("moshi-mumble 0.1");
        msg.set_os("Linux");
        msg.set_os_version("");
        return tcp_send_proto(MSG_Version, msg);
    }

    bool send_authenticate() {
        MumbleProto::Authenticate msg;
        msg.set_username(username_);
        if (!password_.empty()) msg.set_password(password_);
        msg.set_opus(true);
        msg.set_client_type(1); // BOT
        return tcp_send_proto(MSG_Authenticate, msg);
    }

    // -----------------------------------------------------------------------
    // Receive thread — dispatches incoming messages
    // -----------------------------------------------------------------------
    void recv_loop() {
        uint8_t header[6];
        while (connected_) {
            if (!ssl_read_exact(header, 6)) {
                if (connected_) fprintf(stderr, "mumble: connection lost\n");
                connected_ = false;
                return;
            }

            uint16_t type = ((uint16_t)header[0] << 8) | header[1];
            uint32_t len  = ((uint32_t)header[2] << 24) | ((uint32_t)header[3] << 16) |
                            ((uint32_t)header[4] << 8)  | header[5];

            std::vector<uint8_t> payload(len);
            if (len > 0 && !ssl_read_exact(payload.data(), (int)len)) {
                if (connected_) fprintf(stderr, "mumble: connection lost during payload\n");
                connected_ = false;
                return;
            }

            handle_message(type, payload.data(), len);
        }
    }

    void handle_message(uint16_t type, const uint8_t *data, uint32_t len) {
        switch (type) {
        case MSG_Version: {
            MumbleProto::Version msg;
            msg.ParseFromArray(data, (int)len);
            printf("mumble: server version: %s\n",
                   msg.has_release() ? msg.release().c_str() : "unknown");
            break;
        }
        case MSG_ServerSync: {
            MumbleProto::ServerSync msg;
            msg.ParseFromArray(data, (int)len);
            session_id_ = msg.session();
            if (msg.has_welcome_text()) {
                printf("mumble: %s\n", msg.welcome_text().c_str());
            }
            synced_ = true;
            sync_cv_.notify_all();
            break;
        }
        case MSG_Reject: {
            MumbleProto::Reject msg;
            msg.ParseFromArray(data, (int)len);
            fprintf(stderr, "mumble: rejected: %s\n",
                    msg.has_reason() ? msg.reason().c_str() : "unknown");
            connected_ = false;
            synced_ = false;
            sync_cv_.notify_all();
            break;
        }
        case MSG_ChannelState: {
            MumbleProto::ChannelState msg;
            msg.ParseFromArray(data, (int)len);
            if (msg.has_name() && msg.has_channel_id()) {
                std::lock_guard<std::mutex> lock(channels_mutex_);
                channel_map_[msg.name()] = msg.channel_id();
            }
            break;
        }
        case MSG_UserState: {
            // Could track other users for multi-speaker mixing later
            break;
        }
        case MSG_UDPTunnel: {
            handle_audio_packet(data, len);
            break;
        }
        case MSG_Ping: {
            // Server pong — ignore
            break;
        }
        case MSG_CryptSetup: {
            // We only use TCP tunnel, no UDP encryption needed
            break;
        }
        case MSG_CodecVersion: {
            MumbleProto::CodecVersion msg;
            msg.ParseFromArray(data, (int)len);
            if (msg.has_opus() && msg.opus()) {
                printf("mumble: server supports Opus\n");
            }
            break;
        }
        case MSG_ServerConfig:
        case MSG_PermissionQuery:
        case MSG_TextMessage:
        case MSG_UserRemove:
        case MSG_ChannelRemove:
        case MSG_SuggestConfig:
            // Silently ignore for now
            break;
        default:
            // Unknown message type — ignore
            break;
        }
    }

    // -----------------------------------------------------------------------
    // Audio packet handling (UDPTunnel payload)
    //
    // Format:
    //   byte 0: (type << 5) | target
    //   varint: session ID (server → client only)
    //   varint: sequence number
    //   opus header: varint-encoded (length | terminator_bit<<13)
    //   opus data: <length> bytes
    // -----------------------------------------------------------------------
    void handle_audio_packet(const uint8_t *data, uint32_t len) {
        if (len < 1) return;

        uint8_t header_byte = data[0];
        int audio_type = (header_byte >> 5) & 0x07;
        // int target    = header_byte & 0x1F;

        // Type 4 = Opus
        if (audio_type != 4) return;

        int pos = 1;

        // Session ID (varint)
        uint64_t session_id;
        int n = mumble_varint_decode(data + pos, (int)len - pos, session_id);
        if (n < 0) return;
        pos += n;

        // Skip our own audio
        if (session_id == session_id_) return;

        // Sequence number (varint)
        uint64_t seq;
        n = mumble_varint_decode(data + pos, (int)len - pos, seq);
        if (n < 0) return;
        pos += n;

        // Opus header (varint): lower 13 bits = length, bit 13 = terminator
        uint64_t opus_header;
        n = mumble_varint_decode(data + pos, (int)len - pos, opus_header);
        if (n < 0) return;
        pos += n;

        int opus_len   = (int)(opus_header & 0x1FFF);
        // bool terminator = (opus_header & 0x2000) != 0;

        if (pos + opus_len > (int)len) return;
        if (opus_len == 0) return;

        // Decode Opus → PCM float
        float pcm[OPUS_FRAME_SIZE * 2]; // some headroom
        int samples = opus_decode_float(opus_dec_, data + pos, opus_len,
                                        pcm, OPUS_FRAME_SIZE * 2, 0);
        if (samples <= 0) return;

        // Append to RX buffer
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_pcm_buf_.insert(rx_pcm_buf_.end(), pcm, pcm + samples);

        // Cap buffer at ~1 second to prevent unbounded growth
        const size_t max_samples = (size_t)MOSHI_SAMPLE_RATE;
        if (rx_pcm_buf_.size() > max_samples) {
            rx_pcm_buf_.erase(rx_pcm_buf_.begin(),
                              rx_pcm_buf_.end() - max_samples);
        }
    }

    // -----------------------------------------------------------------------
    // Send Opus audio as UDPTunnel
    //
    // Format:
    //   byte 0: (4 << 5) | 0    // type=Opus, target=normal
    //   varint: sequence number
    //   varint: (opus_len | terminator_bit)
    //   opus data
    // -----------------------------------------------------------------------
    void send_opus_packet(const uint8_t *opus_data, int opus_len, bool terminator) {
        uint8_t pkt[OPUS_MAX_PACKET + 32];
        int pos = 0;

        // Header byte: type=4 (Opus), target=0 (normal talk)
        pkt[pos++] = (4 << 5) | 0;

        // Sequence number
        pos += mumble_varint_encode(pkt + pos, tx_sequence_++);

        // Opus header: length | (terminator << 13)
        uint64_t opus_hdr = (uint64_t)opus_len;
        if (terminator) opus_hdr |= 0x2000;
        pos += mumble_varint_encode(pkt + pos, opus_hdr);

        // Opus payload
        memcpy(pkt + pos, opus_data, opus_len);
        pos += opus_len;

        // Send as UDPTunnel (type 1) — raw bytes, not protobuf
        tcp_send(MSG_UDPTunnel, pkt, (uint32_t)pos);
    }

    // -----------------------------------------------------------------------
    // Channel join
    // -----------------------------------------------------------------------
    void join_channel(const std::string &name) {
        // Wait a moment for channel list to populate
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        std::lock_guard<std::mutex> lock(channels_mutex_);
        auto it = channel_map_.find(name);
        if (it == channel_map_.end()) {
            fprintf(stderr, "mumble: channel \"%s\" not found. Available:\n", name.c_str());
            for (auto &kv : channel_map_) {
                fprintf(stderr, "  \"%s\" (id=%u)\n", kv.first.c_str(), kv.second);
            }
            return;
        }

        MumbleProto::UserState msg;
        msg.set_session(session_id_);
        msg.set_channel_id(it->second);
        tcp_send_proto(MSG_UserState, msg);
        printf("mumble: joining channel \"%s\" (id=%u)\n", name.c_str(), it->second);
    }

    // -----------------------------------------------------------------------
    // Ping thread — keep-alive every 15 seconds
    // -----------------------------------------------------------------------
    void ping_loop() {
        while (connected_) {
            auto now = std::chrono::steady_clock::now();
            auto ts  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch()).count();

            MumbleProto::Ping msg;
            msg.set_timestamp((uint64_t)ts);
            tcp_send_proto(MSG_Ping, msg);

            for (int i = 0; i < 150 && connected_; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    std::string host_;
    int         port_ = 0;
    std::string username_;
    std::string password_;
    std::string channel_;

    int          sock_fd_  = -1;
    SSL_CTX     *ssl_ctx_  = nullptr;
    SSL         *ssl_      = nullptr;

    OpusEncoder *opus_enc_ = nullptr;
    OpusDecoder *opus_dec_ = nullptr;

    std::atomic<bool> connected_{false};
    std::atomic<bool> synced_{false};
    uint32_t session_id_ = 0;

    std::mutex              sync_mutex_;
    std::condition_variable sync_cv_;

    std::thread recv_thread_;
    std::thread ping_thread_;

    // TX
    std::mutex tx_mutex_;
    uint64_t   tx_sequence_ = 0;

    // RX audio buffer
    std::mutex         rx_mutex_;
    std::deque<float>  rx_pcm_buf_;

    // Channel map: name → id
    std::mutex                              channels_mutex_;
    std::map<std::string, uint32_t>         channel_map_;
};
