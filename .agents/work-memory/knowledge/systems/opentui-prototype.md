# opentui-prototype 系统档案

OpenTUI 重写验证原型：bun 1.3.14 + @opentui/core 0.5.1（Zig 核心 + TS 绑定），Windows 11。
目标：验证 OpenTUI 能否承载 com_lite_client 的聊天客户端重写（消息即时显示、协议兼容）。
由 2026-08-11 OpenTUI 评估任务建立；状态：验证通过，是否全量重写待 sensei 拍板。

## 结构

| 文件 | 职责 |
| --- | --- |
| src/protocol.ts | 协议层 TS 移植，与 C++ common/ 逐字节等价（帧头 6B：opcode/status u8 + body_len u32 BE；帧体紧凑 JSON，键序 from_uid_,to_uid_,chat_type_,client_msg_id_,msg_type_,content_；max_message_body_length=65530） |
| src/client.ts | Bun socket 封装：ChatClient（connect/login/send/nextFrame/close），分帧复用 takeFrame，异步回调模型无线程无锁 |
| src/main.ts | OpenTUI 最小聊天 TUI：ScrollBox 消息列表 + Input 输入框 + 状态行；收帧循环 + pending 匹配 |
| src/smoke.ts | 无 UI 冒烟脚本：连接→登录→自发自收验证 |

## 运行

```bash
bun install
bun src/main.ts <uid> <passwd> [to] [host] [port]   # TUI（默认 127.0.0.1:7891）
bun src/smoke.ts <uid> <passwd> [host] [port]        # 冒烟
```

服务端演示用户表（com_lite 仓库 src/server/main.cpp）：alice/alice123、bob/bob456、neuroil/123、whr/123。

## 使用时的注意事项

- **工厂函数 vs 实例 API**：`Text()/Box()/Input()` 工厂返回 ProxiedVNode（声明式 vnode 代理），属性赋值（`t.content = "x"`）**静默无效**；动态更新必须 `new TextRenderable(renderer, opts)` 拿真实实例。静态布局可用工厂。
- **Input 回车默认不提交**：0.5.1 默认键位 `return → newline`（单行 Input 吞掉），提交要 `meta+return`（文档与实现不符）；解法：自定义 keyBindings 或全局 `keypress` 里 `input.submit()`（本原型用后者）。
- **Esc 退出兜底**：`exitOnCtrlC` 只销毁渲染器不退出进程（上游 issue #1355）；DESTROY 事件里 `client.close()` + `setImmediate(() => process.exit(0))` 兜底，且所有 UI 写入（appendLine/setStatus）过 `uiDead` 守卫（来自 bugs/2026-08-11-textbuffer-destroyed.md）。
- **渲染调度**：实例 API 下动态更新正常（初始渲染 + content setter 触发重渲染）；早期"渲染静止"现象是 vnode 赋值无效所致，非渲染器问题。
- **协议层长度语义必须是 UTF-8 字节**：JS `string.length` 是 UTF-16 码元（中文 1 码元=3 字节），写 body_len / 分配缓冲 / 65530 校验一律先 `TextEncoder().encode()` 拿字节数组（来自 bugs/2026-08-11-chinese-message-blocks.md）；`encodeInto` 返回值不检查会静默截断。
- 打包单文件 exe（`bun build --compile`）是否兼容 @opentui/core 的 native 模块**未验证**。
