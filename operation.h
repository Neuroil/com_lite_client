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

constexpr int  kRecvTimeoutSec = 5;
constexpr std::size_t kNoBodyCap        = static_cast<std::size_t>(-1); // 不截断帧体打印
constexpr std::size_t kBriefBodyBytes   = 128; // 双进程场景帧体可达 64KiB，只打印前若干字节
constexpr int         kTickSec          = 1;   // 接收方每次 recv 的超时片，用于逼近驻留时限
constexpr int         kHoldSecDefault   = 60;  // 接收方默认驻留秒数（等人工启动发送方）
constexpr int         kSenderHoldSec    = 10;  // 发送方发完后默认驻留秒数（观察是否被杀）
// 防误输护栏：基准场景单次 1M 条已远超需要，更大的 count 几乎必是手误 —— count 只驱动发送
// 循环（无 OOM），但会让本次运行时长任意拉长，纯属自伤。
constexpr std::size_t kMaxSenderCount = 1'000'000;
// 入站累积缓冲上限：防恶意对端声明超大 body_len 后慢速灌数据（帧体永不凑齐，acc 只进不出、
// 无界增长）。1MiB 对齐服务端 kWriteHighWater 语义；合法帧最大 6+65530=65536B，此处是其
// 16 倍，不会误伤正常流量。
constexpr std::size_t kMaxAccBytes = 1 * 1024 * 1024;

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
inline bool set_recv_timeout(SOCKET fd, int sec) {
    const DWORD ms = static_cast<DWORD>(sec) * 1000;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
}

inline bool pre_login(
    SOCKET             fd,
    const std::string& uid,
    const std::string& credential,
    std::uint8_t&      out_status
) {
    out_status = static_cast<std::uint8_t>(types::Status::_init_);

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

    std::byte     chunk[4096];
    const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
    if (n < static_cast<int>(FrameHeader::wire_size)) {
        std::cerr << "[pre-login] no login response (closed / timeout)\n";
        return false;
    }
    out_status = static_cast<std::uint8_t>(chunk[1]);
    std::cout << "[pre-login] login response: opcode=" << static_cast<int>(chunk[0])
            << " status=" << static_cast<int>(chunk[1]) << "\n";
    return true;
}

inline bool login_as(SOCKET fd, const std::string& uid, const std::string& credential) {
    std::uint8_t status = 0;
    if (!pre_login(fd, uid, credential, status)) {
        return false;
    }
    if (status != static_cast<std::uint8_t>(types::Status::ok)) {
        std::cerr << "[login] uid=" << uid << " rejected (status=" << static_cast<int>(status)
                << ")\n";
        return false;
    }
    std::cout << "[login] uid=" << uid << " logged in\n";
    return true;
}

// 从累积缓冲解析完整帧并打印（帧头字段 + 帧体字节，供人工比对）
// 帧边界按「wire_size + 4 字节大端 body_len」判定；帧体未齐则保留字节继续等
// count 为已解析帧总数（跨多次调用累计，帧序号据此续排）
// label 是打印用的帧别（M1-M6 是自身回显，M8-M10 收的是服务端转发/响应）
// body_cap 限制帧体打印字节数，kNoBodyCap 表示原样全打
inline void drain_frames(std::vector<std::byte>& acc, int& count, const char* label,
                  std::size_t body_cap) {
    while (acc.size() >= FrameHeader::wire_size) {
        std::uint32_t body_len = 0;
        std::memcpy(&body_len, acc.data() + 2, sizeof(body_len));
        body_len = ntohl(body_len);

        const std::size_t frame_len = FrameHeader::wire_size + body_len;
        if (acc.size() < frame_len) {
            break; // 帧体未齐
        }

        const std::size_t print_len = body_len < body_cap ? body_len : body_cap;
        std::cout << "  " << label << " frame #" << (count + 1)
                  << ": opcode=" << static_cast<int>(acc[0])
                  << " status=" << static_cast<int>(acc[1])
                  << " body_len=" << body_len
                  << " body=";
        std::cout.write(
            reinterpret_cast<const char*>(acc.data() + FrameHeader::wire_size), print_len);
        if (print_len < body_len) {
            std::cout << "...(+" << (body_len - print_len) << "B)";
        }
        std::cout << std::endl;

        acc.erase(acc.begin(), acc.begin() + static_cast<std::ptrdiff_t>(frame_len));
        ++count;
    }
}

// 带超时循环 recv，直到凑齐 want 个完整帧（或连接关闭 / 超时）。
// 返回：>= 0 为实际收到的完整帧数；-1 = 超时未凑齐；-2 = 连接关闭或 recv 错误。
inline int recv_frames(SOCKET fd, std::vector<std::byte>& acc, int want) {
    int count = 0; // 跨多次 recv 累计的完整帧总数
    for (;;) {
        drain_frames(acc, count, "echo", kNoBodyCap);
        if (count >= want) {
            return count;
        }

        std::byte chunk[4096];
        const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n > 0) {
            acc.insert(acc.end(), chunk, chunk + n);
            // 合法流量下 acc 峰值 = 一个未凑齐的帧残余（< 65536B）+ 本次 4096B；超过
            // kMaxAccBytes 只可能来自恶意对端声明超大 body_len 后慢速灌数据（帧永不凑齐）。
            if (acc.size() > kMaxAccBytes) {
                std::cout << "  inbound buffer exceeded " << kMaxAccBytes
                          << "B (peer declared oversized body_len?) -- treating as connection error\n";
                return -2;
            }
            continue;
        }
        if (n == 0) {
            std::cout << "  connection closed by peer (EOF), got " << count
                      << " of " << want << " frames\n";
            return -2;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            std::cout << "  recv timeout (" << kRecvTimeoutSec << "s), got " << count
                      << " of " << want << " frames\n";
            return -1;
        }
        std::cout << "  recv error: " << wsa_last_error_string() << "\n";
        return -2;
    }
}

// M5/M6：等服务端关闭连接，把 EOF / recv 错误当作预期结果。
// 返回 0 = 连接已关（预期）；1 = 超时仍连接（未预期，说明服务端没有杀连接）。
// acc 只为与 recv_frames 保持同一组调用签名而保留：这里等的是 close，
// 收到的残余数据直接打印、不入缓冲区。
inline int recv_expect_close(SOCKET fd, [[maybe_unused]] std::vector<std::byte>& acc) {
    for (;;) {
        std::byte chunk[4096];
        const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n == 0) {
            std::cout << "  connection closed by peer (EOF) -- expected\n";
            return 0;
        }
        if (n > 0) {
            std::cout << "  unexpected data before close: ";
            std::cout.write(reinterpret_cast<const char*>(chunk), n);
            std::cout << std::endl;
            continue; // 有数据先收完，仍然等 close
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            std::cout << "  recv timeout: connection still open -- not expected\n";
            return 1;
        }
        // WSAECONNRESET 等错误也是"服务端关连接"的一种表现（close 未读净数据 → RST）
        std::cout << "  recv error: " << wsa_last_error_string()
                  << " -- treated as expected close\n";
        return 0;
    }
}

// 十进制无符号命令行参数解析。空串、含非数字字符、溢出一律判非法（返回 false，不改 out）。
inline bool parse_uint(const std::string& s, std::size_t& out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    try {
        out = std::stoull(s);
    } catch (...) { // out_of_range
        return false;
    }
    return true;
}

// 非阻塞排空自身入站字节并打印已凑齐的帧。
// 返回 false = 对端已关闭或 recv 出错（此时残留字节先解完再报）；true = 超时/无数据可读。
inline bool pump_inbound(SOCKET fd, std::vector<std::byte>& acc, int& in_count) {
    for (;;) {
        std::byte     chunk[4096];
        const int n = recv(fd, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
        if (n > 0) {
            acc.insert(acc.end(), chunk, chunk + n);
            drain_frames(acc, in_count, "recv", kBriefBodyBytes); // 每轮就地解帧，acc 不无界增长
            // 恶意对端声明超大 body_len 后慢速灌数据时帧永不凑齐，解帧后仍有残留 → 无界增长。
            if (acc.size() > kMaxAccBytes) {
                std::cout << "  inbound buffer exceeded " << kMaxAccBytes
                          << "B (peer declared oversized body_len?) -- treating as connection error\n";
                return false;
            }
            continue;
        }
        if (n == 0) {
            drain_frames(acc, in_count, "recv", kBriefBodyBytes);
            std::cout << "  connection closed by peer (EOF) after " << in_count
                      << " inbound frame(s)\n";
            return false;
        }
        const int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            return true;
        }
        std::cout << "  recv error: " << wsa_last_error_string() << "\n";
        return false;
    }
}


#endif //COM_LITE_CUSTOM_OPERATION_H
