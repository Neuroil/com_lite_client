//
// Created by Neuroil on 2026/7/23.
//

#ifndef COM_LITE_DECODER_H
#define COM_LITE_DECODER_H

#include <WinSock2.h>

#include "common/binary_code.h"
#include "common/message_pack.h"
#include "json.hpp"
#include "common/io_status.h"
#include "common/message.h"
#include "common/frame_header.h"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace codec {
    class Decoder {
    public:
        // 分帧逻辑应当在 decoder 之外就完成了
        static types::IoStatus decode(
            const std::vector<std::byte>& in_buf,
            MessagePack&                  out_rmp
        ) noexcept {
            try {
                // 帧头未齐：保留字节等待更多数据，返回 incomplete（与协议错误区分开）。
                if (in_buf.size() < FrameHeader::wire_size) return types::IoStatus::incomplete;

                // 枚举值域校验先于 static_cast
                const auto raw_opcode = static_cast<std::uint8_t>(in_buf[0]);
                const auto raw_status = static_cast<std::uint8_t>(in_buf[1]);
                if (!types::is_known_opcode(raw_opcode) || !types::is_known_status(raw_status)) {
                    return types::IoStatus::error;
                }

                std::uint32_t body_len = 0;
                std::memcpy(&body_len, in_buf.data() + 2, sizeof(body_len));
                body_len = ntohl(body_len);

                // 承重次序 1：frame_too_long 校验必须先于帧体完整性校验。
                // 违反它会让 incomplete 携带一个未经校验的 body_len，读缓冲重新变成无界——
                // 即引入 incomplete 所要消灭的 bug 换条路重建。
                if (body_len > max_message_body_length) return types::IoStatus::frame_too_long;

                // 承重次序 2：in_buf.size() - wire_size 的无符号下溢不可达，因为帧头未齐检查（:31）已先返回。
                // 减法写法是有意的（原为防 32 位平台上 body_len 接近 UINT32_MAX 时右侧溢出）；
                // 订正：该防溢出理由已随上面的上限校验失效（6 + body_len 在 32 位上不可能溢出），
                // 写法保留无害，靠的是顺序而非自身。
                // 帧体未齐：保留字节等待更多数据，返回 incomplete。
                if (in_buf.size() - FrameHeader::wire_size < body_len) return types::IoStatus::incomplete;

                const std::string s(
                    reinterpret_cast<const char*>(in_buf.data()) + FrameHeader::wire_size,
                    body_len
                );

                nlohmann::json j = nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
                if (j.is_discarded()) return types::IoStatus::error; // 必须调方法，不能与 value_t 比较
                if (!j.is_object()) return types::IoStatus::error;   // "[1,2,3]" / "null" 会通过 parse

                Message msg = j.get<Message>(); // 不用 get_to：失败时不半写出参
                if (!msg.is_valid()) return types::IoStatus::error;

                FrameHeader fh;
                fh.opcode_   = static_cast<types::Opcode>(raw_opcode);
                fh.status_   = static_cast<types::Status>(raw_status);
                fh.body_len_ = body_len;

                // 执行最终操作（不能再抛异常）
                out_rmp.fh_  = fh;
                out_rmp.msg_ = std::move(msg);
                return types::IoStatus::ok;
            } catch (...) {
                return types::IoStatus::error;
            }
        }
    };
} // codec

#endif //COM_LITE_DECODER_H
