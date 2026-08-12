---
id: 2026-08-11-textbuffer-destroyed
symptom: OpenTUI 客户端 Ctrl+C 退出时必现崩溃 "TextBuffer is destroyed"
affected_systems: [opentui-prototype]
from_task: -
fixed: 2026-08-11
verified: yes
---

# OpenTUI Ctrl+C 退出崩溃：微任务续体写入已销毁的 TextBuffer

## 症状

opentui-prototype（bun 1.3.14 + @opentui/core 0.5.1，Windows 11）：Ctrl+C 退出时**必现**崩溃，异常 `TextBuffer is destroyed`（TextBuffer.guard 在 `_destroyed` 时 throw）。崩溃发生在退出流程中，进程以 exit 1 终止（非干净退出）。

## 调用堆栈（代表性）

```
error: TextBuffer is destroyed
      at guard (node_modules/@opentui/src/text-buffer.ts:42:32)
      at setStyledText (node_modules/@opentui/src/text-buffer.ts:86:10)
      at updateTextBuffer (node_modules/@opentui/src/renderables/Text.ts:49:21)
      at content (node_modules/@opentui/src/renderables/Text.ts:80:12)   ← content setter，无 isDestroyed 守卫
      at appendLine (src/main.ts:93:3)                                    ← 收帧循环退出后的 UI 写入
      at <anonymous> (src/main.ts:139:7)                                  ← 收帧循环 async IIFE 续体
```

## 边界清单（关键）

- **finalizeDestroy 顺序**：`cleanupBeforeDestroy()` → `emit("destroy")` → `root.destroyRecursively()`（TextBuffer.destroy 置 `_destroyed=true`）→ native 清理。`emit("destroy")` 与组件销毁在同一同步栈内。
- **DESTROY 回调（应用层）**：`client.close()` 同步执行，`rejectAll()` 把收帧循环 `nextFrame()` 的 await 续体以 resolve(null) 排入**微任务队列**；`setImmediate(process.exit(0))` 排入**宏任务队列**。
- **微任务先于宏任务**：同步栈结束后微任务先 drain，`process.exit` 永远轮不到——崩溃抢先于退出。
- **Ctrl+C 入口**：keypress `c+ctrl` → `process.nextTick(() => this.destroy())`（nextTick 队列先于 promise 微任务 drain，时序不变）。

## 静态可疑点

1. main.ts 收帧循环退出后的 `appendLine("[system] connection closed")`（main.ts:139→93）——destroy 后唯一可达的 UI 写入路径。
2. `statusText.content = "disconnected"`（原 main.ts:140）——同属循环续体，appendLine 修好后此处同样会炸，须一并收口。
3. TextRenderable / TextBufferRenderable 的 `set content` **均无 isDestroyed 守卫**（同库 MarkdownRenderable.set content 有守卫，index.bun.js:11395——库内不一致）。

## 逻辑调用链

| 步 | 事件 | 微任务队列 | 宏任务队列 |
|---|---|---|---|
| 1 | stdin data：Ctrl+C keypress，`nextTick(destroy)` 入队 | — | — |
| 2 | nextTick drain：`destroy()` → `finalizeDestroy()` | — | — |
| 3 | `cleanupBeforeDestroy()`：移除 uncaughtException/unhandledRejection 兜底、pause stdin | — | — |
| 4 | `emit("destroy")` → main.ts:30 `client.close()`：rejectAll → resolve(null) | **[1] 收帧循环续体** | — |
| 5 | main.ts:31 `setImmediate(process.exit(0))` | [1] | **[A] process.exit** |
| 6 | 同栈继续：`root.destroyRecursively()` → listText 的 TextBuffer `_destroyed=true` | [1] | [A] |
| 7 | native 清理 → 栈结束 | [1] | [A] |
| 8 | 微任务 drain：续体 `frame===null → continue → while(client.connected)` false → 退出循环 | — | [A] |
| 9 | `appendLine("connection closed")` → content setter（无守卫）→ `setStyledText` → **guard throw** | — | [A] |
| 10 | unhandled rejection → Bun 默认终止（exit 1） | — | [A] 永不执行 |

其他 destroy 后写 UI 路径检查：ENTER 回调不可达（库的 keypressHandler 首行 `if (this._isDestroyed) return`，stdin 已 pause）；handleFrame/onData 在 close 后无数据源；渲染循环本身带 isDestroyed 检查。login 窗口内 Ctrl+C 走同款崩溃（同源变体，由守卫一并覆盖）。

## 根因

（sensei 选定的假设 A：应用层微/宏任务竞态）

DESTROY 回调把进程退出推迟到宏任务（setImmediate），但 `client.close()` 的 rejectAll 把收帧循环续体排进微任务——微任务必然先于宏任务，续体在 TextBuffer 已销毁后执行 appendLine 写 UI，而库的 TextRenderable set content 无 isDestroyed 守卫，使 destroy 后写 UI 从静默 no-op 变成 throw。三层因果：emit("destroy") 先于组件树销毁（触发条件）× 微/宏任务顺序（时序）× setter 无守卫（崩溃直接原因）。

## 修复

- `src/main.ts`：模块级 `let uiDead = false`；DESTROY 回调置 `uiDead = true`（保留 `client.close()` + `setImmediate(process.exit(0))`）；`appendLine` 开头 `if (uiDead) return;`；新增 `setStatus(text)` 收口（内部 `if (uiDead) return; statusText.content = text;`），替换全部 4 处 `statusText.content = ...` 写入。
- `src/client.ts`：实例级 `private closed = false`；`close()` 开头置 `closed = true`；`connect()` 的 await 续体检查 `closed`，已关闭则 `socket.end()` 并 return（堵僵尸连接：Ctrl+C 时 Bun.connect 微任务续体可能先于 setImmediate 执行并复活 this.socket）。
- `src/main.ts`：keypress 全局回调与 ENTER 回调开头补 `if (uiDead) return;`（防御性收口）。
- 相关测试 / 验证方式：启动 → 登录 → 发消息（功能正常）→ 一次 Ctrl+C → 进程 exit 0 干净退出、无崩溃、终端恢复。修复前必现崩溃，修复后多次验证不复现。
- 横向搜索结果（lateral-search）：similar_problems=4（1 真实同模式：client.ts connect 僵尸连接，已修；1 防御缺口：input 写入未过守卫，已修；2 提示级 C++ 侧，不属本 bug 同类，未动）。C++ 侧无「销毁后访问」同类风险（ocp#2/ocp#7 结构规避）。
- 网络查证（network-research）：
  - **PR #882**（anomalyco/opentui，merged 2026-06-25）"fix(core): guard lifecycle passes loop against destroyed renderables"——官方对 "TextBuffer is destroyed" 的唯一合并修复，只改生命周期循环，**未覆盖 set content 路径**（0.5.1 发布于 8-04，晚于该 PR，故我们命中的路径仍无守卫）。
  - **opencode #34083**（open）同款真实世界崩溃（setProp→setDefaultFg→guard），证明未守卫 setter 仍在野外崩溃。
  - **#1355**（open，2026-08-11）"exitHandler destroys the renderer but never exits"——同 exit 路径的相反症状（不崩、不死），workaround 是 onDestroy 回调。
  - **#1319**（open）destroy 后异步续体继续跑的上游已知模式，与本 bug 根因同构。
  - 官方立场：写入点加 isDestroyed 守卫即可（#882 作者实测"守卫足够、destroy 重排不必要"）——与本次应用层守卫方向一致，无更优官方方案。

## 防复发要点（写给未来读者）

1. **OpenTUI 的 DESTROY 事件语义是"开始销毁"，不是"销毁完成"**——回调里禁止做任何会唤醒 await 续体的操作（close/rejectAll 会排微任务）；若必须，同时置应用层 destroyed 标志并让所有 UI 写入过守卫。
2. **所有异步续体写 UI 前必须检查销毁状态**（appendLine/setStatus 单口收口 + 守卫）；任何新回调路径（ENTER、收帧、定时器）加入时先问"destroy 后能到达吗"。
3. **库的 setter 守卫不可依赖**：TextRenderable.set content 无 isDestroyed 守卫（0.5.1）；升级 @opentui/core 时检查上游是否给 setter 补了守卫（PR #882 只补了循环），补了可考虑简化应用层守卫。
4. **退出路径用宏任务兜底时，注意微任务续体必然先跑**——不要在退出回调里同时做"唤醒续体"和"延迟退出"两件事。

## 相关

- knowledge/systems/opentui-prototype.md（本 bug 建立）
- 上游：https://github.com/anomalyco/opentui/pull/882 、 https://github.com/anomalyco/opentui/issues/1355 、 https://github.com/anomalyco/opencode/issues/34083
