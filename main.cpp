//
// Created by Neuroil on 2026/8/10.
//

// utils
#include <iostream>
#include <string>


// socket
#include <WinSock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")

// GBK to UTF-8
#include <windows.h>

// basis
#include "common/io_status.h"
#include "common/message.h"
#include "common/message_pack.h"
#include "common/encoder.h"
#include "common/decoder.h"

#include "operation.h"

constexpr auto kServerAddress = "129.211.215.72";
constexpr int  kServerPort    = 7891;


int main() {
    // 1. 设置控制台输出和输入代码页为 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 建立 socket 连接
    WSADATA wsaData;
    int     result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        std::cout << "WSAStartup failed: " << result << std::endl;
        return 1;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        std::cout << "socket failed with error: " << WSAGetLastError() << std::endl;
        WSACleanup();
        return 1;
    }


    sockaddr_in service{};
    service.sin_family = AF_INET;
    inet_pton(AF_INET, /*kServerAddress*/ "127.0.0.1", &service.sin_addr);
    service.sin_port = htons(kServerPort);

    result = connect(fd, reinterpret_cast<SOCKADDR*>(&service), sizeof(service));
    if (result == SOCKET_ERROR) {
        std::cout << "connect failed with error: " << WSAGetLastError() << std::endl;
        closesocket(fd);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected to server." << std::endl; //连接成功，输出消息


    std::string uid;
    std::string passwd;

    std::cout << "uid: ";
    if (!(std::cin >> uid)) {
        std::cerr << "\nfailed to set uid\n";
        return 1;
    }

    std::cout << "\npassword: ";
    if (!(std::cin >> passwd)) {
        std::cerr << "\nfailed to set password\n";
        return 1;
    }

    // 登录
    bool res = login_as(fd, uid, passwd);

    std::vector<std::byte> acc(65536);
    std::vector<std::byte> send_buffer(65536);

    while (res) {
        std::cout << "to: ";
        std::string to_uid;
        if (!(std::cin >> to_uid)) {
            std::cerr << "invalid input\n- Terminated -";
            break;
        }

        std::cout << "content: ";
        std::string content;
        if (!(std::cin >> content)) {
            std::cerr << "invalid input\n- Terminated -";
            break;
        }

        std::vector<std::byte> frame;
        if (!encode_frame(
            make_message(uid.c_str(), to_uid.c_str(), "test", content.c_str()),
            types::Opcode::request,
            types::Status::ok,
            frame
        )) {
            std::cout << "\nFailed to encode message\n";
            continue;
        }
        std::cout << "sending single frame (" << frame.size() << " bytes)\n";
        if (!send_all(fd, frame.data(), static_cast<int>(frame.size()))) {
            std::cout << "\nFailed to send\n";
            continue;
        }

        // 接收 res
        recv(fd, reinterpret_cast<char*>(acc.data()), static_cast<int>(acc.size()), 0);
        MessagePack     rmp;
        types::IoStatus dst = codec::Decoder::decode(acc, rmp);
        // 目前还没有做此类防护
        // if (dst == types::IoStatus::incomplete) break;
        if (dst != types::IoStatus::ok) {
            std::cout << "failed to decode\n";
            continue;
        }

        if (rmp.fh_.status_ == types::Status::fail) std::cout << "target offline\n";
        else std::cout << "success\n";



    }

    closesocket(fd);
    WSACleanup();
    return 0;
}
