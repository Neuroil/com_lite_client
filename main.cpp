//
// Created by Neuroil on 2026/8/10.
//

// utils
#include <atomic>
#include <functional>
#include <iostream>
#include <string>
#include <thread>


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
    inet_pton(AF_INET, kServerAddress, &service.sin_addr);
    service.sin_port = htons(kServerPort);

    result = connect(fd, reinterpret_cast<SOCKADDR*>(&service), sizeof(service));
    if (result == SOCKET_ERROR) {
        std::cout << "connect failed with error: " << WSAGetLastError() << std::endl;
        closesocket(fd);
        WSACleanup();
        return 1;
    }

    // 心跳仅 TCP keepalive（决策 2：无应用层心跳）
    int keep = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keep), sizeof(keep));

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

    // 入站队列在登录前创建：pre_login 在 login 响应解码成功后（无论 status ok/fail）
    // 即把与登录响应同批到达的残留完整帧逐个入队，收线程启动后由主循环统一消费；
    // 仅 decode 失败 / 超时等异常路径不触碰队列。
    InboundQueue q;

    // 登录：无限重试直到成功（决策 6）；提示后输入 q 退出
    for (;;) {
        if (login_as(fd, q, uid, passwd)) {
            break;
        }
        std::cout << "login failed. 输入 q 退出，回车重试\n";
        std::string choice;
        if (!(std::cin >> choice)) {
            std::cerr << "\ninput error - Terminated\n";
            closesocket(fd);
            WSACleanup();
            return 1;
        }
        if (choice == "q") {
            closesocket(fd);
            WSACleanup();
            return 0;
        }
    }

    PendingMap             pending;                        // 待确认响应表（仅主线程读写，ocp#7）
    std::atomic<bool>      stop(false);                    // 请求收线程退出
    std::atomic<ConnState> state(ConnState::connected);     // 收线程连接状态
    // 收线程严格在 pre_login 成功之后启动：登录响应已消费完毕，不会出现双线程抢收
    std::thread recv_thread(recv_loop, fd, std::ref(q), std::ref(stop), std::ref(state));

    while (state.load() == ConnState::connected) {
        // 先排空入站队列（提示语之前，避免输出与输入提示交错）
        drain_inbound(q, pending, uid);

        std::cout << "to: ";
        std::string to_uid;
        if (!(std::cin >> to_uid)) {
            std::cerr << "\ninput error - Terminated\n";
            break;
        }

        std::cout << "content: ";
        std::string content;
        if (!(std::cin >> content)) {
            std::cerr << "\ninput error - Terminated\n";
            break;
        }

        // 发送前复查连接状态：收线程可能已因对端关闭 / 重置而退出
        if (state.load() != ConnState::connected) {
            std::cout << "\nconnection lost, stopping\n";
            break;
        }

        const std::string msg_id = make_client_msg_id();
        pending[msg_id] = PendingEntry{to_uid, content};
        if (pending.size() > kMaxPending) { // 有界自愈：超限删最早（其响应视为丢失）
            pending.erase(pending.begin());
        }

        std::vector<std::byte> frame;
        if (!encode_frame(
            make_message(uid.c_str(), to_uid.c_str(), msg_id.c_str(), content.c_str()),
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
            break;
        }
    }

    // 统一清理块（ocp#2）：所有 break 路径都汇入此处——
    // stop → shutdown 解阻塞收线程 → join → closesocket → WSACleanup。
    // 不留任何未 join 的 std::thread 析构路径。
    stop.store(true);
    shutdown(fd, SD_BOTH);
    recv_thread.join();
    closesocket(fd);
    WSACleanup();
    return 0;
}
