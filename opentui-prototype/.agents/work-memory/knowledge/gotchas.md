# Gotchas（跨任务坑清单）

2026-08-12 更新（任务 2026-08-12-inputbox-clipboard）。锚点相对 node_modules/@opentui/core/ 或项目根。

## @opentui/core 剪贴板

- `copyToClipboardOSC52` 返回 true ≠ 复制成功：仅表示输出路径被调用；OSC52 不支持时静默 return false（chunk-bun-83ry0rzt.js:10568）。别拿返回值当终端已接受。
- OSC52 是写单行道：WT 支持写入（PR #5823，v1.4/1.5+）[外部查证]，不支持读回。应用内"粘贴"走 bracketed paste，别指望 OSC52 读。
- 剪贴板 API 只在 CliRenderer 上（renderer.d.ts:538），RenderContext 没有；`ctx.clipboard` 会撞 private 字段（renderer.d.ts:297），只能走 `(ctx as CliRenderer).copyToClipboardOSC52(...)`。
- Textarea 的 deleteSelection() 内部已 clearSelection（chunk-bun-8fkgaxc6.js:5836-5840/5862-5868），再手动调 ctx.clearSelection() 是重复操作。

## 终端键盘拦截

- `exitOnCtrlC` 默认 true（chunk-bun-8fkgaxc6.js:7251）：不设 false 应用永远收不到 ctrl+c。测试渲染器同样默认 true，测试里按 ctrl+c 会被吞。
- WT 终端级拦截 Ctrl+Shift+C/V，应用内快捷键只能用不带 Shift 的 Ctrl 组合（与本任务 Ctrl+C/X 一致）。
- 覆写 handleKeyPress 记得先查 `traits.suspend === true`（index.bun.js:8384 上游行为），否则挂起态会漏键。

## 测试渲染器

- `mockInput.typeText` 与 `pressKey` 背靠背调用存在异步竞态（typeText 内部有间隔/flush），需在两次调用间留 flush 间隔，否则输入可能错位（本轮契约验证 21/21 实测观察）。
- `createTestRenderer` 默认 exitOnCtrlC:true，测 ctrl+c 路径必须显式传 false。

## 工程

- bun build 默认 target 在此环境失败（浏览器 target 无 node polyfill）；用 `bun build --target=bun --external @opentui/core-*`。
- @opentui/core 的 d.ts / chunk 行号随版本漂移：升级后先 grep 锚点再信档案。
- 本项目 tsconfig 开 noImplicitOverride + verbatimModuleSyntax：覆写必须写 override 关键字，类型导入必须 inline `import type`，漏了就是编译错。
