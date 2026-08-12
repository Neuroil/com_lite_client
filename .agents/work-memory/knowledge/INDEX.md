# Knowledge Index

com_lite_client（Winsock TCP 聊天客户端）知识档案索引。
由 /work 流水线 Stage 5 knowledge-distill 首次建立（2026-08-11，并发收发改造收尾）。

## 档案地图

| 文件 | 内容 | 最近更新 |
| --- | --- | --- |
| systems/com-lite-client.md | 项目系统档案：架构、线程模型、线程所有权表、协议事实、决策记录、未决事项 | 2026-08-11 |
| systems/opentui-prototype.md | OpenTUI 重写验证原型：结构、运行方式、使用注意事项（vnode/回车/退出兜底） | 2026-08-11 |
| bugs/2026-08-11-textbuffer-destroyed.md | OpenTUI Ctrl+C 退出崩溃：微任务续体写入已销毁 TextBuffer；应用层 uiDead 守卫 | 2026-08-11 |
| bugs/2026-08-11-chinese-message-blocks.md | 中文消息阻塞：encodeFrame 用 UTF-16 码元数当字节数，帧体截断触发服务端断连；改字节语义 | 2026-08-11 |
| gotchas.md | 跨任务坑清单（Winsock / 线程 / 协议 / 工程） | 2026-08-11 |

## 阅读指引

- 新任务先读 systems/com-lite-client.md——线程所有权表是并发改造后的核心约束。
- 改动 Winsock / 线程 / 协议相关代码前必读 gotchas.md。
- 服务端协议事实标注 [外部查证]（隔壁服务端仓库查得）；改服务端前需重新核实。

## 惯例

- 锚点统一 `文件:行`，文件相对项目根；行号随代码演进可能漂移，以内容为准。
- 事实以 git 状态与代码为准；推断条目标注 [INFERENCE]。
- 长期资产，保持中性技术文风，不带人格语气。
