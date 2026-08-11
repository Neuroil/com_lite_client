//
// Created by Neuroil on 2026/7/15.
//

#ifndef COM_LITE_IO_STATUS_H
#define COM_LITE_IO_STATUS_H

namespace types {
    enum class IoStatus {
        ok,
        closed,
        would_block,
        interrupted,
        frame_too_long,
        timeout,
        error,
        incomplete,
    };

    // 面向日志 / 诊断的可读名称。永不返回 nullptr。
    inline const char* to_string(IoStatus s) noexcept {
        switch (s) {
            case IoStatus::ok:              return "Ok";
            case IoStatus::closed:          return "Closed";
            case IoStatus::would_block:     return "WouldBlock";
            case IoStatus::interrupted:     return "Interrupted";
            case IoStatus::frame_too_long:  return "FrameTooLong";
            case IoStatus::timeout:         return "Timeout";
            case IoStatus::error:           return "Error";
            case IoStatus::incomplete:      return "Incomplete";
        }
        return "Unknown";
    }
}

#endif //COM_LITE_IO_STATUS_H