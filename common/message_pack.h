//
// Created by Neuroil on 2026/7/22.
//

#ifndef COM_LITE_MESSAGE_PACK_H
#define COM_LITE_MESSAGE_PACK_H

#include <utility>

#include "common/message.h"
#include "common/frame_header.h"


struct MessagePack {
    FrameHeader fh_;
    Message     msg_;

    MessagePack() = default;

    explicit MessagePack(Message request_msg) {
        fh_  = {};
        msg_ = std::move(request_msg);
    }

    explicit MessagePack(FrameHeader fh) : fh_(fh) {
        msg_ = {};
    }

    explicit MessagePack(FrameHeader fh, Message request_msg) : fh_(fh) {
        msg_ = std::move(request_msg);
    }

    // 同样的 is_init .其实我觉得 is_valid 这个名字好像更好一点
    // 已经改了
    bool is_valid() const { return fh_.is_valid() && msg_.is_valid(); }
};


#endif //COM_LITE_MESSAGE_PACK_H
