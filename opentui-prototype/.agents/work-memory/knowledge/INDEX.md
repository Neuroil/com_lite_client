# Knowledge Index

opentui-prototype（@opentui/core TUI 聊天客户端原型）知识档案索引。
由 /work 流水线 Stage 5 knowledge-distill 建立（2026-08-12，任务 2026-08-12-inputbox-clipboard：InputBox 剪贴板 Ctrl+C/Ctrl+X/粘贴原生）。

## 档案地图

| 文件 | 内容 | 最近更新 |
| --- | --- | --- |
| systems/opentui-inputbox.md | 系统档案：InputBox 组件契约 + @opentui/core@0.5.1 剪贴板/键位/测试 API 事实（含 file:line 锚点） | 2026-08-12 |
| gotchas.md | 跨任务坑清单（OSC52 语义 / exitOnCtrlC / 测试渲染器竞态 / 构建 target） | 2026-08-12 |

## 阅读指引

- 改 InputBox 或任何 Textarea 派生组件前先读 systems/opentui-inputbox.md——handleKeyPress 覆写契约与剪贴板 API 路径是核心约束。
- 涉及剪贴板 / 终端键盘拦截 / 测试渲染器时必读 gotchas.md。
- @opentui/core 锚点均指向 node_modules/@opentui/core@0.5.1 实际行号；升级 core 后行号会漂移，以内容为准。

## 惯例

- 锚点统一 `文件:行`，文件相对项目根（node_modules 锚点相对 node_modules/@opentui/core/）；行号随版本演进可能漂移，以内容为准。
- 事实以 git 状态与代码为准；推断/外部查证条目标注 [INFERENCE] 或 [外部查证]。
- 长期资产，保持中性技术文风，不带人格语气。
