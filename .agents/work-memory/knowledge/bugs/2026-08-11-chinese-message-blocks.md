---
id: 2026-08-11-chinese-message-blocks
symptom: 发送中文消息后客户端完全阻塞无法收发，服务端打印 Disconnect 掉线
affected_systems: [opentui-prototype]
from_task: -
fixed: 2026-08-11
verified: yes
---

# 中文消息阻塞：encodeFrame 用 UTF-16 码元数当字节数，帧体截断触发服务端断连

## 症状

opentui-prototype（bun 1.3.14 + @opentui/core 0.5.1，Windows 11）客户端：英文消息收发正常；发送**含中文**的消息后，客户端完全无法收发（收帧循环空转、send 无响应），服务端（com_lite C++）打印 `Client disconnected` 掉线。

## 调用堆栈（代表性）

无崩溃堆栈（协议层字节错误，服务端断开连接）。代表性调用链：

```
main.ts ENTER 回调 → client.send() → encodeFrame(protocol.ts) → Bun socket write
                                                     ↓ 帧体截断 + body_len 偏小
服务端 recv → take_frame 按 body_len 切帧 → Decoder::decode → JSON parse 失败
                                                     ↓
core.cpp:192 fatal=true → close_client(fd) → main.cpp:38 打印 "Client disconnected"
```

## 边界清单（关键）

- **编码边界（bug 所在）**：`serialized.length` 是 UTF-16 码元数；线上字节是 UTF-8（中文 1 码元 = 3 字节）。C++ 侧 `std::string::size()` 是字节数——TS 移植时单元错配。
- **encodeInto 码点边界**：缓冲不足时在完整码点边界停止（不写半个汉字），尾部 0x00 残留；返回值 `{read, written}` 从未被检查。
- **65530 上限不对称**：TS 按码元、C++ 按字节校验（旧代码的第二个错误面）。
- **服务端对坏帧零容忍**：decode 任何非 ok（error/frame_too_long）→ fatal → 整连接关闭；incomplete 则挂起等待。
- **decode 侧对称**：客户端 TextDecoder 按 body_len 字节切片、服务端 body_len 字节准确——收中文正常，不对称只在 TS encodeFrame 一处。

## 静态可疑点

1. protocol.ts encodeFrame：`new Uint8Array(6 + serialized.length)` + `encodeInto(serialized, out.subarray(6))` 返回值未检查——根因，实锤。
2. 同处 65530 校验语义与 C++ 不一致（修复时连同改为字节语义）。
3. 客户端收帧循环轮询模式（5s 超时）——断连感知延迟的表象，非协议 bug。

## 逻辑调用链

| content | UTF-16 码元 | UTF-8 字节 | 缓冲 6+len | encodeInto 实际写入 | body_len 字段 | 服务端行为 |
|---|---|---|---|---|---|---|
| `hello`（全 ASCII） | 113 | 113 | 119 | 113（写满） | 113 | 正常 parse ✓ |
| `你好` | 110 | 114 | 116 | **109**（106 ASCII + 你 3B；好需 3B 只剩 1B → 码点边界停） | **110** | 按 body_len=110 切 → 缺 1 字节 → **incomplete 挂起**；客户端再发消息时帧头字节被拼入 → parse error → 断开 |
| `好`（单字） | 109 | 111 | 115 | 109（恰好写满但 JSON 不完整） | 109 | 帧完整但 JSON 截断（缺 `"}`）→ parse error → **立即断开** |

不变量：shortfall = 2 × 非 ASCII 字符数 ≥ 2，任何含汉字的帧体都是非法 JSON，parse 必失败（单字立即断开，多字挂起后断开，两条路径最终同一症状）。

## 根因

（sensei 选定的假设 A：encodeFrame 单元错配）

encodeFrame 用 UTF-16 码元数（`serialized.length`）同时做帧体缓冲分配、body_len 字段写入与 65530 上限校验。JS string.length 是 UTF-16 码元：ASCII 1 码元 = 1 字节，中文 1 码元 = 3 UTF-8 字节。`TextEncoder.encodeInto` 写满目标缓冲即在码点边界静默停止（不抛错），返回值从未被检查 → 中文帧体截断、body_len 与实际字节不一致 → 服务端按 body_len 切出非法/不完整 JSON → 解析失败 → 服务端协议违约即断开连接 → 客户端收帧循环退出，表象为"完全无法收发"。英文纯 ASCII 码元数 = 字节数所以正常。

## 修复

- `opentui-prototype/src/protocol.ts` encodeFrame（80-97 行）：先 `const bytes = new TextEncoder().encode(serialized)` 拿完整 UTF-8 字节；65530 上限校验改 `bytes.length`（与 C++ encoder.h `serialized.size()` 对齐）；缓冲 `new Uint8Array(6 + bytes.length)`；body_len 写 `bytes.length`；`out.set(bytes, 6)` 拷入。用 encode() 而非 encodeInto，消灭"返回值未检查"整类错误。decode 侧（decodeFrame/takeFrame）字节语义本就正确，未动。
- 相关测试 / 验证方式：冒烟脚本（英文）回归正常；一次性中文验证脚本（内容 `你好，世界！中文测试 hello 混合`）→ 登录成功、self-echo 逐字还原、delivered 正常；TUI 端到端连续发送两条中文（`中文测试消息，能通吗`、`第二条，服务端没断吧`）→ 本地即时显示、连接持续存活（修复前发中文即断开）。验证脚本已删除。
- 横向搜索结果（lateral-search）：similar_problems=0，recommendation=none。encodeFrame 是全库唯一"码元数当字节数"错配点（全库 grep `.length|substring|encodeInto|byteLength` 无其他命中）；C++ 侧 std::string 本身就是字节容器，结构上不可能复现；四端（JS encode / C++ encode / JS decode / C++ decode）65530 校验现已统一为 UTF-8 字节语义。
- 网络查证（network-research）：
  - WHATWG Encoding 规范（https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto）：encodeInto 逐码点编码、剩余空间装不下整个码点字节序列即 break（码点边界截断的规范性保证）、read 按 UTF-16 码元计 / written 按字节计、不抛异常——本次 bug 机制与规范逐字吻合，属调用方误用而非库缺陷。
  - MDN（https://developer.mozilla.org/zh-CN/docs/Web/API/TextEncoder/encodeInto）：UTF-8 编码所需空间 ∈ [s.length, s.length×3] 字节，CJK≈3 倍——`string.length` 当字节数给中文必然不够。
  - bun 文档与规范逐字一致（https://bun.com/reference/globals/TextEncoder/encodeInto）；oven-sh/bun#36373（node:util encodeInto 参数校验差异）旁证 bun 追踪 encodeInto 对齐——非 bun 缺陷。
  - 修复方案（TextEncoder.encode 拿字节数组）即 MDN/规范推荐的标准做法，无更优替代。

## 防复发要点（写给未来读者）

1. **JS string.length 是 UTF-16 码元数，不是字节数、也不是字符数**——凡是往线上协议写长度字段、分配字节缓冲、做字节上限校验的地方，一律先 `new TextEncoder().encode(str)` 拿字节数组再用 `.length`。
2. **TextEncoder.encodeInto 的返回值（read/written）必须检查**——缓冲不足时它静默截断（码点边界），不抛异常；要么用一次性产出完整字节的 `encode()`，要么按 MDN 循环重分配。
3. **协议移植时长度语义逐项对齐 C++ 参照**：C++ `std::string::size()` 是字节，TS 移植后每个 `.length` 都要问"这里是码元还是字节"；65530 上限在两端必须是同一语义（UTF-8 字节）。
4. 服务端对畸形帧零容忍（协议违约即断开）——客户端任何编码错误都表现为整连接掉线，排查时先怀疑发送侧字节正确性。

## 相关

- knowledge/systems/opentui-prototype.md（注意事项段已追加编码字节语义条目）
- 类似历史 bug：knowledge/bugs/2026-08-11-textbuffer-destroyed.md（同日、同原型、异步时序类）
