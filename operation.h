//
// Created by Neuroil on 2026/8/10.
//

#ifndef COM_LITE_CUSTOM_OPERATION_H
#define COM_LITE_CUSTOM_OPERATION_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <iostream>
#include <cstring>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <utility>
#include <iomanip>
#include <sstream>

#include <WinSock2.h>
// windows.h 必须在 WinSock2.h 之后：检测到 _WINSOCK2API_ 后不会再拉入 winsock.h
#ifndef NOMINMAX
#define NOMINMAX // 防止 min/max 宏污染 std::min/std::max
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib,"ws2_32.lib") // MSVC 下自动链接；MinGW 需自行加 -lws2_32
#endif

#include "common/binary_code.h"
#include "common/io_status.h"
#include "common/decoder.h"
#include "common/encoder.h"
#include "common/frame_header.h"
#include "common/message.h"
#include "common/message_pack.h"

constexpr int         kRecvTimeoutSec    = 5;    // 登录响应等阻塞 recv 的超时秒数
// 入站累积缓冲上限：防恶意对端声明超大 body_len 后慢速灌数据（帧体永不凑齐，acc 只进不出、
// 无界增长）。1MiB 对齐服务端 kWriteHighWater 语义；合法帧最大 6+65530=65536B，此处是其
// 16 倍，不会误伤正常流量。
constexpr std::size_t kMaxAccBytes       = 1 * 1024 * 1024;
constexpr std::size_t kMaxInboundFrames  = 256; // 入站队列上限，满则丢最旧帧
constexpr std::size_t kMaxPending        = 256; // 待确认响应表上限，超则删最早条目
constexpr int         kRecvTickMs        = 500; // 收线程每次 recv 的超时片（毫秒），用于响应 stop

inline Message make_message(const char* from_uid, const char* to_uid, const char* msg_id, const char* content) {
    Message r{};
    r.from_uid_      = from_uid;
    r.to_uid_        = to_uid;
    r.chat_type_     = types::ChatTypes::single;
    r.msg_type_      = types::MessageTypes::text;
    r.client_msg_id_ = msg_id;
    r.content_       = content;
    return r;
}

// 编码一帧，返回整帧字节。encode() 的 out_buf 可能大于 out_len（复用缓冲残留），
// 这里截断到 out_len，保证后续拼接不含残留字节。失败返回 false。
inline bool encode_frame(
    const Message&          msg,
    types::Opcode           opcode,
    types::Status           status,
    std::vector<std::byte>& out
) {
    FrameHeader fh{};
    fh.opcode_ = opcode;
    fh.status_ = status;
    MessagePack pack(fh, msg);

    uint32_t out_len = 0;
    if (codec::Encoder::encode(pack, out, out_len) != types::IoStatus::ok) {
        return false;
    }
    out.resize(out_len);
    return true;
}

// 发送完整段（小帧一次系统调用即可发完，这里按返回值循环以防部分写）。
inline bool send_all(SOCKET fd, const std::byte* data, int len) {
    int sent = 0;
    while (sent < len) {
        const int n = send(fd, reinterpret_cast<const char*>(data) + sent, len - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

// 最近一次 Winsock 错误的可读描述（替代 POSIX 的 strerror(errno)）。
inline std::string wsa_last_error_string() {
    const int   err = WSAGetLastError();
    char*       buf = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err), 0, reinterpret_cast<char*>(&buf), 0, nullptr);
    std::string out;
    if (buf != nullptr) {
        out.assign(buf, len);
        LocalFree(buf);
    }
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) {
        out.pop_back(); // FormatMessage 自带行尾换行
    }
    return "WSA error " + std::to_string(err) + (out.empty() ? "" : ": " + out);
}

// Windows 的 SO_RCVTIMEO 取值是 DWORD 毫秒，不是 POSIX 的 timeval 结构。
inline bool set_recv_timeout(SOCKET fd, int ms) {
    const DWORD d = static_cast<DWORD>(ms);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&d), sizeof(d)) == 0;
}

enum class FrameResult { ok, incomplete, error };

// 纯分帧器：从累积缓冲切出一帧，不涉及任何解码。
// 校验次序钉死（ocp#1）：body_len 上限检查必须先于帧体完整性检查——否则 incomplete
// 会携带一个未经校验的 body_len，读缓冲可能重新变得无界。
// acc.size() < wire_size（帧头未齐）或帧体未齐 → incomplete，acc 保持不变；
// body_len 超限 → error（协议违约，直接判死）；否则 out_frame 装完整帧、acc 前移。
inline FrameResult take_frame(std::vector<std::byte>& acc, std::vector<std::byte>& out_frame) {
    if (acc.size() < FrameHeader::wire_size) {
        return FrameResult::incomplete; // 帧头未齐，还读不到 body_len
    }
    std::uint32_t body_len = 0;
    std::memcpy(&body_len, acc.data() + 2, sizeof(body_len));
    body_len = ntohl(body_len);

    if (body_len > max_message_body_length) { // ocp#1：上限检查先于完整性检查
        return FrameResult::error;
    }

    const std::size_t frame_len = FrameHeader::wire_size + body_len;
    if (acc.size() < frame_len) {
        return FrameResult::incomplete; // 帧体未齐，acc 保持不变
    }

    out_frame.assign(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(frame_len));
    acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(frame_len));
    return FrameResult::ok;
}

// 收线程 → 主线程的唯一共享通道（ocp#7）：frames_ / dropped_ 由 mutex_ 保护。
// push 仅收线程调用，pop / size 仅主线程调用。
struct InboundQueue {
    mutable std::mutex      mutex_; // mutable：const size() 也需加锁
    std::deque<MessagePack> frames_;
    std::size_t             dropped_ = 0; // 满队列被丢弃的帧数（主线程诊断打印用）

    // 满 kMaxInboundFrames 丢最旧帧并计数，随后入队。正常情形永不失败。
    bool push(MessagePack&& rmp) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.size() >= kMaxInboundFrames) {
            frames_.pop_front();
            ++dropped_;
        }
        frames_.push_back(std::move(rmp));
        return true;
    }

    bool pop(MessagePack& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            return false;
        }
        out = std::move(frames_.front());
        frames_.pop_front();
        return true;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }
};

// 收线程连接状态机（std::atomic 承载，主线程轮询读取）。
enum class ConnState { connected, peer_closed, peer_reset, recv_error, buf_overflow };

// 客户端消息 id：GetTickCount64() 高 44 位 + 自增序号低 20 位（& 0xFFFFF 回绕）。
// 定宽 16 位小写十六进制 → 字典序即时间序（ocp#6）。主线程独占调用，无锁安全。
inline std::string make_client_msg_id() {
    static std::uint64_t seq = 0;
    const std::uint64_t  raw =
        (static_cast<std::uint64_t>(GetTickCount64()) << 20) | (seq++ & 0xFFFFF);
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << raw;
    return oss.str();
}

struct PendingEntry {
    std::string to_uid;
    std::string content;
};
// 待确认响应表：client_msg_id → 发送时的目标 / 内容。有界 kMaxPending，超限删最早。
// 仅主线程读写（ocp#7）：收线程绝不触碰，响应匹配只在 drain_inbound 里做。
using PendingMap = std::map<std::string, PendingEntry>;

// 收线程主体：acc / frame / chunk 均线程私有，唯一共享的是 q（InboundQueue）。
// 严禁访问 PendingMap 或任何主线程数据结构（ocp#7）。
inline void recv_loop(
    SOCKET                   fd,
    InboundQueue&            q,
    std::atomic<bool>&       stop,
    std::atomic<ConnState>&  state
) {
    set_recv_timeout(fd, kRecvTickMs); // 500ms 超时片，便于响应 stop / shutdown

    std::vector<std::byte> acc;
    std::vector<std::byte> frame;
    std::byte              chunk[4096];

    while (!stop.load()) {
        const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n > 0) {
            acc.insert(acc.end(), chunk, chunk + n);
            // 合法流量下 acc 峰值 = 一个未凑齐的帧残余（< 65536B）+ 本次 4096B；超过
            // kMaxAccBytes 只可能来自恶意对端声明超大 body_len 后慢速灌数据（帧永不凑齐）。
            if (acc.size() > kMaxAccBytes) {
                state.store(ConnState::buf_overflow);
                std::cout << "[recv] inbound buffer exceeded " << kMaxAccBytes
                          << "B (peer declared oversized body_len?) -- stopping\n";
                break;
            }
            for (;;) {
                const FrameResult fr = take_frame(acc, frame);
                if (fr == FrameResult::incomplete) {
                    break; // 帧头 / 帧体未齐，等下一片数据
                }
                if (fr == FrameResult::error) {
                    state.store(ConnState::buf_overflow); // error 唯一来源：body_len 超限
                    std::cout << "[recv] frame body_len exceeds max_message_body_length\n";
                    return;
                }
                MessagePack rmp;
                if (codec::Decoder::decode(frame, rmp) != types::IoStatus::ok) {
                    state.store(ConnState::recv_error);
                    std::cout << "[recv] decode error: " << wsa_last_error_string() << "\n";
                    return;
                }
                q.push(std::move(rmp));
            }
            continue;
        }
        if (n == 0) {
            state.store(ConnState::peer_closed);
            std::cout << "[recv] connection closed by peer (EOF)\n";
            break;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            if (stop.load()) {
                break; // 主线程已请求停止（shutdown 后首个超时片）
            }
            continue;
        }
        if (err == WSAECONNRESET) {
            state.store(ConnState::peer_reset);
            std::cout << "[recv] connection reset by peer (RST)\n";
            break;
        }
        if (err == WSAEINTR) {
            continue;
        }
        if (stop.load()) break; // 主线程已请求停止（shutdown 后 recv 返回 WSAESHUTDOWN）
        state.store(ConnState::recv_error);
        std::cout << "[recv] recv error: " << wsa_last_error_string() << "\n";
        break;
    }
}

// 登录：发送 login 帧后阻塞收取 login 响应。走 take_frame + Decoder::decode 消费
// 恰好一帧（ocp#3）；同批到达的残留完整帧逐个解码入队（收线程启动后由主循环消费），
// 残帧直接丢弃——残帧丢弃：登录态边界外无解码上下文。
inline bool pre_login(
    SOCKET             fd,
    const std::string& uid,
    const std::string& credential,
    InboundQueue&      q,
    MessagePack&       out_rmp
) {
    if (!set_recv_timeout(fd, kRecvTimeoutSec * 1000)) {
        std::cerr << "[pre-login] failed to set recv timeout\n";
        return false;
    }

    Message msg;
    msg.chat_type_     = types::ChatTypes::single;
    msg.msg_type_      = types::MessageTypes::text;
    msg.from_uid_      = uid;
    msg.to_uid_        = uid;
    msg.client_msg_id_ = "pre-login";
    msg.content_       = credential; // 与服务端 main.cpp 演示用户表一致

    std::vector<std::byte> frame;
    if (!encode_frame(msg, types::Opcode::login, types::Status::ok, frame)) {
        std::cerr << "[pre-login] failed to encode login frame\n";
        return false;
    }
    if (!send_all(fd, frame.data(), static_cast<int>(frame.size()))) {
        std::cerr << "[pre-login] failed to send login frame\n";
        return false;
    }

    std::vector<std::byte> acc;
    std::vector<std::byte> one_frame;
    std::byte              chunk[4096];
    for (;;) {
        const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n <= 0) {
            std::cerr << "[pre-login] no login response (closed / timeout)\n";
            return false;
        }
        acc.insert(acc.end(), chunk, chunk + n);

        const FrameResult fr = take_frame(acc, one_frame);
        if (fr == FrameResult::incomplete) {
            continue; // 帧头 / 帧体未齐，继续收
        }
        if (fr == FrameResult::error) {
            std::cerr << "[pre-login] oversized login response frame\n";
            return false;
        }
        // ok：恰好消费一帧 login 响应
        if (codec::Decoder::decode(one_frame, out_rmp) != types::IoStatus::ok) {
            std::cerr << "[pre-login] failed to decode login response\n";
            return false;
        }
        std::cout << "[pre-login] login response: opcode=" << static_cast<int>(out_rmp.fh_.opcode_)
                  << " status=" << static_cast<int>(out_rmp.fh_.status_) << "\n";

        // 残留完整帧逐个 decode 入队；残帧直接丢弃——残帧丢弃：登录态边界外无解码上下文。
        for (;;) {
            const FrameResult fr2 = take_frame(acc, one_frame);
            if (fr2 == FrameResult::incomplete) {
                break; // 残帧丢弃：登录态边界外无解码上下文
            }
            if (fr2 == FrameResult::error) {
                std::cerr << "[pre-login] oversized trailing frame, discarding rest\n";
                break;
            }
            MessagePack rmp;
            if (codec::Decoder::decode(one_frame, rmp) == types::IoStatus::ok) {
                q.push(std::move(rmp));
            }
        }
        return true;
    }
}

inline bool login_as(SOCKET fd, InboundQueue& q, const std::string& uid, const std::string& credential) {
    MessagePack rmp;
    if (!pre_login(fd, uid, credential, q, rmp)) {
        return false;
    }
    if (rmp.fh_.status_ != types::Status::ok) {
        std::cerr << "[login] uid=" << uid << " rejected (status="
                  << static_cast<int>(rmp.fh_.status_) << ")\n";
        return false;
    }
    std::cout << "[login] uid=" << uid << " logged in\n";
    return true;
}

// 主线程 UI：排空入站队列并分类打印（ocp#7）。pending 仅主线程访问，收线程不触碰。
// response → 匹配待确认表，按 status 打印投递结果并 erase；request → 完整打印
//（发给自己显示 (self) 副本）；ack / login / 未知 → 诊断行。
inline void drain_inbound(InboundQueue& q, PendingMap& pending, const std::string& self_uid) {
    MessagePack rmp;
    while (q.pop(rmp)) {
        const Message& msg = rmp.msg_;
        switch (rmp.fh_.opcode_) {
            case types::Opcode::response: {
                PendingMap::iterator it = pending.find(msg.client_msg_id_);
                if (it != pending.end()) {
                    const bool delivered = rmp.fh_.status_ == types::Status::ok;
                    std::cout << "→ to: " << it->second.to_uid
                              << (delivered ? " delivered" : " failed") << "\n";
                    pending.erase(it);
                } else {
                    std::cout << "unmatched response: id=" << msg.client_msg_id_ << "\n";
                }
                break;
            }
            case types::Opcode::request:
                if (msg.from_uid_ == self_uid) {
                    std::cout << "(self) " << msg.content_ << "\n"; // 发给自己：服务端回显副本
                } else {
                    std::cout << "[" << msg.from_uid_ << "] " << msg.content_ << "\n";
                }
                break;
            default: // ack / login / 未知 opcode —— 诊断行
                std::cout << "[inbound] opcode=" << static_cast<int>(rmp.fh_.opcode_)
                          << " status=" << static_cast<int>(rmp.fh_.status_)
                          << " id=" << msg.client_msg_id_ << "\n";
                break;
        }
    }

    // 满队列丢弃计数：pop 完统一报一次并清零（与帧打印不交错）
    std::size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(q.mutex_);
        dropped    = q.dropped_;
        q.dropped_ = 0;
    }
    if (dropped != 0) {
        std::cout << dropped << " inbound frame(s) dropped (queue full)\n";
    }
}


#endif //COM_LITE_CUSTOM_OPERATION_H
