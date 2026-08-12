// 协议层 TypeScript 移植 —— 与 C++ common/ 逐字节等价
// 对照：frame_header.h / binary_code.h / message.h / encoder.h / decoder.h / io_status.h

export const maxMessageBodyLength = 65530;

// binary_code.h
export enum Opcode {
  ack = 0,
  request = 1,
  response = 2,
  login = 3,
  init = 0xff,
}

export enum Status {
  ok = 0,
  fail = 1,
  init = 0xff,
}

// message.h
export enum ChatTypes {
  single = 0,
  group = 1,
  init = 0xff,
}

export enum MessageTypes {
  text = 0,
  pic = 1,
  init = 0xff,
}

export interface Message {
  from_uid_: string;
  to_uid_: string;
  chat_type_: ChatTypes;
  client_msg_id_: string;
  msg_type_: MessageTypes;
  content_: string;
}

// 与 C++ Message::is_valid() 等价
export function messageIsValid(m: Message): boolean {
  return (
    m.chat_type_ !== ChatTypes.init &&
    m.msg_type_ !== MessageTypes.init &&
    m.from_uid_.length > 0 &&
    m.to_uid_.length > 0 &&
    m.client_msg_id_.length > 0 &&
    m.content_.length > 0
  );
}

// encoder.h body_serializer：nlohmann::json dump(-1, ' ', false, strict) == JSON.stringify 紧凑输出。
// 键序必须钉死为声明序：from_uid_, to_uid_, chat_type_, client_msg_id_, msg_type_, content_
export function encodeBody(m: Message): string {
  return JSON.stringify({
    from_uid_: m.from_uid_,
    to_uid_: m.to_uid_,
    chat_type_: m.chat_type_,
    client_msg_id_: m.client_msg_id_,
    msg_type_: m.msg_type_,
    content_: m.content_,
  });
}

// decoder.h 的枚举值域校验
export function isKnownOpcode(raw: number): boolean {
  return raw === Opcode.ack || raw === Opcode.request || raw === Opcode.response || raw === Opcode.login;
}

export function isKnownStatus(raw: number): boolean {
  return raw === Status.ok || raw === Status.fail;
}

// 整帧编码：6 字节帧头（opcode/status u8 + body_len u32 大端）+ JSON 帧体
// 单元错配根因：serialized 是 JS string，其 .length 为 UTF-16 码元数，而帧体、body_len、
// 上限校验全部以 UTF-8 字节为单位（对齐 C++ encoder.h 的 serialized.size()）。ASCII 1 码元
// = 1 字节，但中文等非 ASCII 字符 1 码元 = 3 个 UTF-8 字节。旧实现用 .length 分配缓冲，会
// 偏小；encodeInto 写满目标缓冲即停止（只在完整码点边界截断，不写半码点），其 {read, written}
// 返回值又从未被检查，于是帧体被悄悄截断且 body_len 偏小 → 服务端按 body_len 切出的帧体是
// 非法/不完整 JSON → 解析失败 → 服务端协议违约即断开（Disconnect）。故必须先用
// TextEncoder.encode() 拿到 UTF-8 字节序列，再做分配、校验与写入（encode() 一次性产出完整
// 字节，从根上消灭"返回值未检查"这一整类错误）。
export function encodeFrame(opcode: Opcode, status: Status, msg: Message): Uint8Array | null {
  const serialized = encodeBody(msg);
  const bytes = new TextEncoder().encode(serialized);
  if (bytes.length > maxMessageBodyLength) return null; // frame_too_long

  const out = new Uint8Array(6 + bytes.length);
  out[0] = opcode;
  out[1] = status;
  new DataView(out.buffer).setUint32(2, bytes.length, false); // htonl
  out.set(bytes, 6);
  return out;
}

export type FrameResult = { kind: "incomplete" } | { kind: "error" } | { kind: "ok"; frame: Uint8Array };

// operation.h take_frame 分帧逻辑移植：从累积缓冲切出一帧
// 校验次序：body_len 上限检查必须先于帧体完整性检查
export function takeFrame(acc: Uint8Array): FrameResult {
  if (acc.length < 6) return { kind: "incomplete" };
  const bodyLen = new DataView(acc.buffer, acc.byteOffset, acc.length).getUint32(2, false);
  if (bodyLen > maxMessageBodyLength) return { kind: "error" };
  const frameLen = 6 + bodyLen;
  if (acc.length < frameLen) return { kind: "incomplete" };
  return { kind: "ok", frame: acc.slice(0, frameLen) };
}

export type DecodeResult =
  | { kind: "incomplete" }
  | { kind: "error" }
  | { kind: "ok"; opcode: Opcode; status: Status; bodyLen: number; msg: Message };

// decoder.h decode 移植：帧头校验 + JSON parse + Message 校验
export function decodeFrame(frame: Uint8Array): DecodeResult {
  if (frame.length < 6) return { kind: "incomplete" };
  const rawOpcode = frame[0];
  const rawStatus = frame[1];
  if (!isKnownOpcode(rawOpcode) || !isKnownStatus(rawStatus)) return { kind: "error" };

  const view = new DataView(frame.buffer, frame.byteOffset, frame.length);
  const bodyLen = view.getUint32(2, false);
  if (bodyLen > maxMessageBodyLength) return { kind: "error" };
  if (frame.length - 6 < bodyLen) return { kind: "incomplete" };

  let j: unknown;
  try {
    j = JSON.parse(new TextDecoder().decode(frame.subarray(6, 6 + bodyLen)));
  } catch {
    return { kind: "error" }; // C++ 的 parse(allow_exceptions=false) → discarded → error
  }
  if (typeof j !== "object" || j === null || Array.isArray(j)) return { kind: "error" }; // 必须 is_object

  const o = j as Record<string, unknown>;
  const msg: Message = {
    from_uid_: String(o.from_uid_ ?? ""),
    to_uid_: String(o.to_uid_ ?? ""),
    chat_type_: Number(o.chat_type_) as ChatTypes,
    client_msg_id_: String(o.client_msg_id_ ?? ""),
    msg_type_: Number(o.msg_type_) as MessageTypes,
    content_: String(o.content_ ?? ""),
  };
  if (!messageIsValid(msg)) return { kind: "error" };

  return { kind: "ok", opcode: rawOpcode as Opcode, status: rawStatus as Status, bodyLen, msg };
}
