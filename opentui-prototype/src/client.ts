// Bun socket 客户端封装：连接管理 + 分帧 + 帧回调
// 对应 C++ 的 recv_loop / InboundQueue 职责，异步回调模型下无需线程与互斥

import type {Socket} from "bun";
import {
    decodeFrame,
    encodeFrame,
    takeFrame,
    type DecodeResult,
    type Message,
    type Opcode,
    type Status
} from "./protocol.ts";

export interface ChatClientOptions {
    hostname: string;
    port: number;
    uid: string;
    credential: string;
    /** 登录响应超时（毫秒），对应 C++ kRecvTimeoutSec */
    loginTimeoutMs?: number;
}

export class ChatClient {
    private socket: Socket<null> | null = null;
    private closed = false; // 关闭标志：堵 Esc 时 in-flight 的 connect() 续体复活连接（僵尸连接）
    private acc = new Uint8Array(0); // 累积缓冲（对应 recv_loop 的 acc）
    private buffer: (DecodeResult & { kind: "ok" })[] = []; // 解码完成但未消费的帧
    private waiters: ((r: (DecodeResult & { kind: "ok" }) | null) => void)[] = [];

    readonly uid: string;
    private credential: string;
    private hostname: string;
    private port: number;
    private loginTimeoutMs: number;

    constructor(opts: ChatClientOptions) {
        this.hostname = opts.hostname;
        this.port = opts.port;
        this.uid = opts.uid;
        this.credential = opts.credential;
        this.loginTimeoutMs = opts.loginTimeoutMs ?? 5000;
    }

    get connected(): boolean {
        return this.socket !== null && !this.socket.closed && !this.socket.done;
    }

    async connect(): Promise<void> {
        const socket = await Bun.connect({
            hostname: this.hostname,
            port: this.port,
            socket: {
                data: (_s, chunk) => this.onData(chunk),
                error: (_s, err) => {
                    console.error("[client] socket error:", err);
                    this.rejectAll();
                },
                close: () => this.rejectAll(),
            },
        });
        // Esc 竞态根因：DESTROY 回调里 close() 之后，Bun.connect 的 await 续体（微任务）
        // 仍可能先于 setImmediate(exit)（宏任务）执行——若直接赋回 this.socket 会复活已关闭的
        // 连接（僵尸连接：connected 重新为 true，main.ts 继续往其上发帧泄漏连接）。closed 标志
        // 让续体自检：已关闭则立即关掉刚建立的 socket 并返回，绝不赋回实例。
        if (this.closed) {
            socket.end();
            return;
        }
        this.socket = socket;
    }

    close(): void {
        this.closed = true; // 先置标志：in-flight 的 connect() 续体后续到达时自检并自关
        this.socket?.end();
        this.socket = null;
        this.rejectAll();
    }

    private onData(chunk: Uint8Array): void {
        // 拼接累积缓冲（对应 recv_loop 的 acc.insert）
        const merged = new Uint8Array(this.acc.length + chunk.length);
        merged.set(this.acc);
        merged.set(chunk, this.acc.length);
        this.acc = merged;

        for (; ;) {
            const fr = takeFrame(this.acc);
            if (fr.kind === "incomplete") break;
            if (fr.kind === "error") {
                console.error("[client] protocol error: body_len exceeds max, closing");
                this.close();
                return;
            }
            const dec = decodeFrame(fr.frame);
            if (dec.kind === "error") {
                console.error("[client] decode error, closing");
                this.close();
                return;
            }
            if (dec.kind === "incomplete") {
                // 已切出的完整帧不应 incomplete；防御性处理
                console.error("[client] unexpected incomplete frame");
                this.close();
                return;
            }
            // 从累积缓冲移除已消费帧
            this.acc = this.acc.slice(fr.frame.length);

            const w = this.waiters.shift();
            if (w) {
                w(dec);
            } else {
                this.buffer.push(dec);
            }
        }
    }

    private rejectAll(): void {
        while (this.waiters.length > 0) {
            this.waiters.shift()!(null);
        }
    }

    /** 取下一帧；已缓冲则立即返回，否则等待（单消费者轮询模型，与主循环 drain 等价） */
    nextFrame(timeoutMs: number): Promise<(DecodeResult & { kind: "ok" }) | null> {
        if (this.buffer.length > 0) {
            const f = this.buffer.shift()!;
            return Promise.resolve(f);
        }
        if (!this.connected) return Promise.resolve(null);
        const {promise, resolve} = Promise.withResolvers<(DecodeResult & { kind: "ok" }) | null>();
        const timer = setTimeout(() => {
            const idx = this.waiters.indexOf(wrapped);
            if (idx >= 0) this.waiters.splice(idx, 1);
            resolve(null);
        }, timeoutMs);
        const wrapped: (r: (DecodeResult & { kind: "ok" }) | null) => void = (r) => {
            clearTimeout(timer);
            resolve(r);
        };
        this.waiters.push(wrapped);
        return promise;
    }

    /** 发送整帧；对应 encode_frame + send_all */
    send(opcode: Opcode, status: Status, msg: Message): boolean {
        if (!this.connected || !this.socket) return false;
        const frame = encodeFrame(opcode, status, msg);
        if (frame === null) {
            console.error("[client] frame too long");
            return false;
        }
        this.socket.write(frame);
        return true;
    }

    /** 登录：发送 login 帧后阻塞收取响应（对应 pre_login + login_as） */
    async login(): Promise<boolean> {
        if (!this.connected || !this.socket) return false;
        const msg: Message = {
            from_uid_: this.uid,
            to_uid_: this.uid,
            chat_type_: 0, // single
            client_msg_id_: "pre-login",
            msg_type_: 0, // text
            content_: this.credential,
        };
        if (!this.send(3 /* login */, 0 /* ok */, msg)) return false;
        const resp = await this.nextFrame(this.loginTimeoutMs);
        if (resp === null) {
            console.error("[login] no login response (closed / timeout)");
            return false;
        }
        console.log(`[login] response: opcode=${resp.opcode} status=${resp.status}`);
        return resp.status === 0 /* ok */;
    }
}
