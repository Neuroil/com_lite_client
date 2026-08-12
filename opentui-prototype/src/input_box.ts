import {TextareaRenderable, type CliRenderer, type KeyEvent} from "@opentui/core";

export class InputBox extends TextareaRenderable {
    override handleKeyPress(key: KeyEvent): boolean {
        // Upstream contract: traits.suspend===true skips all key-binding actions (index.bun.js:8384).
        if (this.traits.suspend === true) {
            return super.handleKeyPress(key);
        }
        if (key.ctrl && !key.meta && !key.super && !key.hyper && this.hasSelection()) {
            const text = this.getSelectedText();
            if (key.name === "c") {
                (this.ctx as CliRenderer).copyToClipboardOSC52(text);
                return true;
            }
            if (key.name === "x") {
                const copied = (this.ctx as CliRenderer).copyToClipboardOSC52(text);
                if (copied) {
                    this.deleteSelection();
                }
                // Consume Ctrl+X either way: on copy failure the selection is kept
                // (never delete what was not copied), and super must not see the key.
                return true;
            }
        }
        return super.handleKeyPress(key);
    }
}
