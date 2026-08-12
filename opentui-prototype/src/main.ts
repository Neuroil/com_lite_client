// OpenTUI 最小聊天 TUI 原型（实例 API 版）
// 用法：bun src/main.ts <uid> <passwd> [to] [host] [port]
// 关键教训：Text/Box/Input 工厂返回 ProxiedVNode，动态更新必须用 new XxxRenderable(renderer, opts) 真实实例

import {
    BoxRenderable,
    CliRenderEvents,
    InputRenderable,
    InputRenderableEvents,
    ScrollBoxRenderable,
    TextRenderable,
    createCliRenderer, KeyEvent, TextareaRenderable,
} from "@opentui/core";
import {ChatClient} from "./client.ts";
import type {DecodeResult} from "./protocol.ts";

const [uid, passwd, to = uid, host = "129.211.215.72", portStr = "7891"] = process.argv.slice(2);
if (!uid || !passwd) {
    console.error("usage: bun src/main.ts <uid> <passwd> [to] [host] [port]");
    process.exit(2);
}
const port = Number(portStr);

const renderer = await createCliRenderer({exitOnCtrlC: false});

renderer.keyInput.on("keypress", (key: KeyEvent) => {
    if (key.name === "escape") {
        renderer.destroy()
    }
})

// OpenTUI 0.5.1 的 Esc 只销毁渲染器、不退出进程（上游 issue #1355 同类问题）。
// 渲染器销毁后 socket 与收帧循环仍持有事件循环 → 进程卡住，需二次 Esc（SIGINT 默认处理）。
// 兜底：destroy 事件里断开 socket 并延迟退出，给 native 终端恢复留出事件循环轮次。
// 崩溃根因（TextBuffer is destroyed）：finalizeDestroy 先 emit("destroy") 再销毁组件树，
// 而 destroy 回调里 client.close() 会把收帧循环的 await 续体排入微任务队列，
// setImmediate(process.exit(0)) 是宏任务 → 微任务必先执行，续体在组件已销毁后写 UI
// （listText/statusText 在库内无 isDestroyed 守卫）→ TextBuffer.guard() 抛错。
// 修复：销毁开始即置 uiDead，销毁后的 UI 写入一律静默跳过，退出由 setImmediate 兜底。
let uiDead = false;
renderer.on(CliRenderEvents.DESTROY, () => {
    uiDead = true; // 渲染器开始销毁 → 后续 UI 写入一律跳过
    client.close();
    setImmediate(() => process.exit(0));
});

// 消息列表：真实实例，content 动态更新
const listText = new TextRenderable(renderer, {content: ""});
const scroll = new ScrollBoxRenderable(renderer, {
    stickyStart: "bottom",
    scrollY: true,
    flexGrow: 1,
    borderStyle: "rounded",
    title: ` chat: ${uid} → ${to} `,
});
scroll.content.add(listText);

const input = new TextareaRenderable(renderer, {
    placeholder: "message, Enter to send · Esc to quit\nCtrl+Enter to start a new line",
    flexGrow: 0,
    backgroundColor: "#1a1a1a",
    focusedBackgroundColor: "#2a2a2a",
    // 默认键位里 return 是 newline（被 Input 吞掉），提交要 meta+return；
    // 这里把 return 绑到 submit，恢复"回车即发送"
    keyBindings: [
        {name: "return", action: "submit"},
        {name: "kpenter", shift: false, action: "submit"},
        {name: "backspace", action: "backspace"},
        {name: "left", action: "move-left"},
        {name: "right", action: "move-right"},
        {name: "delete", action: "delete"},
        {name: "up", action: "line-home"},
        {name: "down", action: "line-end"},
        {name: "z", ctrl: true, action: "undo"},
        {name: "y", ctrl: true, action: "redo"},
    ],
});
const statusText = new TextRenderable(renderer, {content: "connecting...", fg: "#888888"});

const rootBox = new BoxRenderable(renderer, {
    flexDirection: "column",
    width: "100%",
    height: "100%",
    padding: 1,
    gap: 1,
});
rootBox.add(scroll);
rootBox.add(input);
rootBox.add(statusText);
renderer.root.add(rootBox);
input.focus();

// 全局兜底：return 键直接触发提交（Input 内部 keybinding 在此版本未按预期生效）
renderer.keyInput.on("keypress", (k: { name?: string }) => {
    if (uiDead) return; // 防御性收口：组件树已销毁后 Input 不可达，但同一防御哲学应保持一致
    if (k.name === "return" && input.value.length > 0) {
        input.submit();
    }
});

const messages: string[] = [];
const pending = new Set<string>(); // 已发送待确认的 msg_id
let seq = 0;

function appendLine(line: string): void {
    if (uiDead) return; // 组件树已销毁：跳过写入，避免 TextBuffer.guard() 崩溃
    messages.push(line);
    if (messages.length > 500) messages.shift();
    listText.content = messages.join("\n");
}

// statusText 唯一写入口：销毁后静默跳过（appendLine 的 guard 拦不住这一处，需单独收口）
function setStatus(text: string): void {
    if (uiDead) return;
    statusText.content = text;
}

function handleFrame(frame: DecodeResult & { kind: "ok" }): void {
    switch (frame.opcode) {
        case 2 /* response */
        : {
            const id = frame.msg.client_msg_id_;
            if (pending.has(id)) {
                pending.delete(id);
                if (frame.status === 0) break;
                appendLine(`→ failed`);
            }
            break;
        }
        case 1 /* request */
        : {
            // 自己消息的服务端回显副本：发送时已本地显示，跳过；否则正常显示
            if (frame.msg.from_uid_ === uid && pending.has(frame.msg.client_msg_id_)) break;
            appendLine(`[${frame.msg.from_uid_}] ${frame.msg.content_}`);
            break;
        }
        default:
            appendLine(`[inbound] opcode=${frame.opcode} status=${frame.status} id=${frame.msg.client_msg_id_}`);
    }
}

// 连接 + 登录
const client = new ChatClient({hostname: host, port, uid, credential: passwd});
try {
    await client.connect();
    appendLine(`[system] connected to ${host}:${port}`);
} catch (e) {
    appendLine(`[system] connect failed: ${(e as Error).message}`);
    setStatus("disconnected");
    process.exitCode = 1;
}
if (client.connected) {
    if (await client.login()) {
        appendLine(`[system] logged in as ${uid}`);
        setStatus(`connected · to: ${to}`);
        // 收帧循环：帧到达立即刷新列表（真实实例 content 更新自动触发重渲染）
        (async () => {
            while (client.connected) {
                const frame = await client.nextFrame(5000);
                if (frame === null) continue;
                handleFrame(frame);
            }
            appendLine("[system] connection closed");
            setStatus("disconnected");
        })();
    } else {
        appendLine("[system] login failed");
        setStatus("login failed");
    }
}

// 发送：Enter → 本地立即显示 + 发帧
input.on(InputRenderableEvents.ENTER, (value: string) => {
    if (uiDead) return; // 防御性收口：销毁后进入此处会写 input.value，单线程下不可达但同一哲学应一致
    const content = value.trim();
    if (content.length === 0) return;
    if (!client.connected) {
        appendLine("[system] not connected");
        return;
    }
    const msgId = `tui-${Date.now().toString(16)}-${seq++}`;
    pending.add(msgId);
    const ok = client.send(1 /* request */, 0 /* ok */, {
        from_uid_: uid,
        to_uid_: to,
        chat_type_: 0 /* single */,
        client_msg_id_: msgId,
        msg_type_: 0 /* text */,
        content_: content,
    });
    if (ok) {
        appendLine(`(self) ${content}`); // 发送即显示，不等待服务端回显
    } else {
        pending.delete(msgId);
        appendLine("[system] send failed");
    }
    input.value = "";
});
