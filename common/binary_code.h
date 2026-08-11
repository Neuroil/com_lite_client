//
// Created by Neuroil on 2026/7/16.
//

#ifndef COM_LITE_BINARY_CODE_H
#define COM_LITE_BINARY_CODE_H
#include <cstdint>

namespace types {
    /* -------------------------------------------------------
     * 定义转换规则
     *
     *      enum                =>         binary code
     * -------------------------------------------------------
     * Opcode::ack              =>              0
     * Opcode::request          =>              1
     * Opcode::response         =>              2
     * Opcode::login            =>              3
     *
     * Status::ok               =>              0
     * Status::fail             =>              1
     *
     * -------------------------------------------------------         *
     */

    enum class Opcode : std::uint8_t {
        _init_   = 0xff,
        ack      = 0,
        request  = 1,
        response = 2,
        login    = 3
    };


    enum class Status : std::uint8_t {
        _init_ = 0xff,
        ok     = 0,
        fail   = 1
    };

    // 线上字节 → 已知枚举值校验。docs/architecture.md §2.4：未知 opcode / status 一律当协议错误拒绝。
    // 有意不写 default:，新增枚举值时由 -Wswitch 提醒此处需同步扩展。
    constexpr bool is_known_opcode(std::uint8_t raw) noexcept {
        switch (static_cast<Opcode>(raw)) {
            case Opcode::ack:
            case Opcode::request:
            case Opcode::response:
            case Opcode::login: return true;
            case Opcode::_init_: return false;
        }
        return false; // 不在枚举列表中的线上字节
    }

    constexpr bool is_known_status(std::uint8_t raw) noexcept {
        switch (static_cast<Status>(raw)) {
            case Status::ok:
            case Status::fail: return true;
            case Status::_init_: return false;
        }
        return false;
    }
} // types

#endif //COM_LITE_BINARY_CODE_H
