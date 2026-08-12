# opentui-inputbox 系统档案

组件：`src/input_box.ts` InputBox（TextareaRenderable 子类，仅实现剪贴板三条快捷键）。
任务：2026-08-12-inputbox-clipboard（方向 B：Ctrl+C 复制 / Ctrl+X 剪切 / 粘贴原生）。
环境：TS + Bun + @opentui/core@0.5.1，Windows 11 + Windows Terminal。

## 组件契约（src/input_box.ts，33 行）

`handleKeyPress(key): boolean` 覆写（input_box.ts:5），判定顺序：

1. `traits.suspend === true` → 直接 `super.handleKeyPress`（input_box.ts:7-9）。
2. `key.ctrl && !meta && !super && !hyper && hasSelection()` 时：
   - `name === "c"` → `copyToClipboardOSC52(getSelectedText())`，return true（选区保留，input_box.ts:12-16）。
   - `name === "x"` → 复制成功才 `deleteSelection()`；失败保选区；两种情况都 return true（不让 super 看见，input_box.ts:17-23）。
3. 其余（含无选区）→ `super.handleKeyPress(key)`（input_box.ts:26）。

注意：**无选区 Ctrl+X 组件不处理**，回退后由宿主应用键位表兜底——本项目 main.ts:83 `{name:"x", ctrl:true, action:"delete"}`（任务前用户配置）。

## @opentui/core 剪贴板 API（事实，已核验）

1. RenderContext 上**没有**剪贴板字段（types.d.ts:93-141 仅 clearSelection/startSelection/updateSelection 等）。正确路径：`(this.ctx as CliRenderer).copyToClipboardOSC52(text)`（renderer.d.ts:538，返回 boolean，可选 ClipboardTarget）。`ctx.clipboard` 双重不合法：RenderContext 无此字段 + CliRenderer.clipboard 是 private（renderer.d.ts:297）。
2. `deleteSelection()` 内部经 `deleteSelectedText()` 已执行 `_ctx.clearSelection()`（chunk-bun-8fkgaxc6.js:5862-5868 → 5836-5840 → 5838）——**勿重复调 ctx.clearSelection**。
3. OSC52 语义：门控在 Clipboard 类 `copyToClipboardOSC52`（chunk-bun-83ry0rzt.js:10568，src/lib/clipboard.ts）——`!isOsc52Supported()` 时静默 return false；CliRenderer.isOsc52Supported 按 `capabilities.osc52_support !== "unsupported"`（chunk-bun-8fkgaxc6.js:9308-9309）。**返回 true 仅表示输出路径被调用，不保证终端接受**。WT 支持 OSC52 写入（PR #5823，v1.4/v1.5+）、不支持读回 [外部查证]。

## 键位系统（事实，已核验）

4. Textarea keyBindings 合并：`mergeKeyBindings(defaultTextareaKeyBindings, custom)`（index.bun.js:8316），custom 覆盖同名键；匹配键 = name:ctrl:shift:meta:super 精确匹配。默认表（index.bun.js:8212+）**无 ctrl+c / ctrl+x**（有 ctrl+a 行首、ctrl+d 删除词等）。
5. tsconfig 约束：`noImplicitOverride:true`（tsconfig.json:23）→ 覆写方法必须写 `override`；`verbatimModuleSyntax:true`（tsconfig.json:15）→ 类型导入必须用 inline `import type` 修饰符（`import {TextareaRenderable, type CliRenderer, type KeyEvent}`）。
6. `exitOnCtrlC`：createCliRenderer 选项默认 true（chunk-bun-8fkgaxc6.js:7251），开启时 ctrl+c 在渲染器层直接退出（7307），应用收不到；本项目 main.ts:25 已设 false。WT 自身默认拦截 Ctrl+Shift+C/V 为终端级复制/粘贴。
7. TextareaRenderable.handleKeyPress 执行顺序（index.bun.js:8383-8407）：traits.suspend!==true 时先查 keyBindings→action→handler（8384-8392）；无绑定且无 ctrl/meta/super/hyper 修饰时插入可打印字符（8395-8406）；否则 return false（8407）。

## 测试渲染器（事实，已核验）

8. `@opentui/core/testing`：`createTestRenderer(options)`（testing/test-renderer.d.ts:54，**默认 exitOnCtrlC:true 需显式 false**）；mockInput：`pressKey(key, {ctrl:true})`（testing/mock-keys.d.ts:36）、`typeText(text, delayMs?)`（:43）、`pressArrow(dir, modifiers?)`（:72）、`pasteBracketedText(text)`（:80）；`setRendererCapabilities(renderer, overrides)`（testing/terminal-capabilities.d.ts:7）。

## 决策记录

- 复制失败（终端不支持 OSC52）时 Ctrl+X 保选区且 return true——宁可不删也不删了没复制上；Ctrl+C 永不删选区。
- 无选区时完全回退 super，组件不吞键。
- 粘贴保持原生（bracketed paste 由 core 处理，handlePaste 不动）。

## 未决事项

- 无。
