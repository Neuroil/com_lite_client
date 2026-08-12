// 无 UI 冒烟脚本：验证协议移植 + 网络收发，对照 C++ 客户端行为
// 用法：bun src/smoke.ts <uid> <passwd> [host] [port]
// 流程：连接 → 登录 → 发一条消息给自己 → 等 (self) 回显 + response → 退出

import { ChatClient } from "./client.ts";

const [uid, passwd, host = "127.0.0.1", portStr = "7891"] = process.argv.slice(2);
if (!uid || !passwd) {
  console.error("usage: bun src/smoke.ts <uid> <passwd> [host] [port]");
  process.exit(2);
}
const port = Number(portStr);

const client = new ChatClient({ hostname: host, port, uid, credential: passwd });

try {
  await client.connect();
  console.log(`[smoke] connected to ${host}:${port}`);
} catch (e) {
  console.error("[smoke] connect failed:", (e as Error).message);
  process.exit(1);
}

if (!(await client.login())) {
  console.error("[smoke] login rejected/failed");
  client.close();
  process.exit(1);
}
console.log(`[smoke] logged in as ${uid}`);

// 发一条消息给自己（对应 C++ 主循环的 request 路径）
const msgId = `smoke-${Date.now().toString(16)}`;
const sent = client.send(
  1 /* request */,
  0 /* ok */,
  {
    from_uid_: uid,
    to_uid_: uid,
    chat_type_: 0 /* single */,
    client_msg_id_: msgId,
    msg_type_: 0 /* text */,
    content_: "hello from opentui prototype",
  },
);
if (!sent) {
  console.error("[smoke] send failed");
  process.exit(1);
}
console.log(`[smoke] sent request id=${msgId}`);

// 收帧：先 (self) 回显（request, from==self），后 response
let sawSelf = false;
let sawResponse = false;
const deadline = Date.now() + 5000;
while (Date.now() < deadline && !(sawSelf && sawResponse)) {
  const remaining = deadline - Date.now();
  const frame = await client.nextFrame(Math.max(100, remaining));
  if (frame === null) break;
  if (frame.opcode === 1 /* request */) {
    console.log(`[smoke] inbound request from=${frame.msg.from_uid_} content="${frame.msg.content_}"`);
    if (frame.msg.from_uid_ === uid) sawSelf = true;
  } else if (frame.opcode === 2 /* response */) {
    console.log(
      `[smoke] response id=${frame.msg.client_msg_id_} status=${frame.status} (${frame.status === 0 ? "delivered" : "failed"})`,
    );
    if (frame.msg.client_msg_id_ === msgId) sawResponse = true;
  } else {
    console.log(`[smoke] other frame opcode=${frame.opcode} status=${frame.status}`);
  }
}

console.log(`[smoke] result: self-echo=${sawSelf ? "yes" : "no"} response=${sawResponse ? "yes" : "no"}`);
client.close();
process.exit(sawSelf && sawResponse ? 0 : 1);
