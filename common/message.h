//
// Created by Neuroil on 2026/7/15.
//

#ifndef COM_LITE_MESSAGE_H
#define COM_LITE_MESSAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include "json.hpp"

inline constexpr std::size_t max_message_body_length = 65530;

namespace types {
    enum class ChatTypes : std::uint8_t {
        _init_ = 0xff,
        single = 0,
        group  = 1
    };

    enum class MessageTypes : std::uint8_t {
        _init_ = 0xff,
        text   = 0,
        pic    = 1
    };
}

// 消息基类
// 原名: MessageBase
struct Message {
    types::ChatTypes    chat_type_ = types::ChatTypes::_init_;
    types::MessageTypes msg_type_  = types::MessageTypes::_init_;
    std::string         from_uid_;
    std::string         to_uid_;
    std::string         client_msg_id_;
    std::string         content_;

    // 原名: is_init()
    bool is_valid() const {
        return chat_type_ != types::ChatTypes::_init_
                && msg_type_ != types::MessageTypes::_init_
                && !from_uid_.empty()
                && !to_uid_.empty()
                && !client_msg_id_.empty()
                && !content_.empty();
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Message, from_uid_, to_uid_, chat_type_, client_msg_id_, msg_type_, content_)


// struct Message : MessageBase {
//     std::string content_;
//
//     bool is_valid() const {
//         return is_init() && !content_.empty();
//     }
// };
//
// NLOHMANN_DEFINE_DERIVED_TYPE_NON_INTRUSIVE(Message, MessageBase, content_)
//
//
// struct ResponseMsg : MessageBase {
//     std::string server_seq_;
//
//     bool is_valid() const {
//         return is_init() && !server_seq_.empty();
//     }
// };
//
// NLOHMANN_DEFINE_DERIVED_TYPE_NON_INTRUSIVE(ResponseMsg, MessageBase, server_seq_)
//
// struct Message : MessageBase {
//     char data_[];
// };
//
// NLOHMANN_DEFINE_DERIVED_TYPE_NON_INTRUSIVE(Message, MessageBase, data_)


#endif //COM_LITE_MESSAGE_H
