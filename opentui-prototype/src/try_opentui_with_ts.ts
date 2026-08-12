import { createCliRenderer, type KeyEvent } from "@opentui/core"

const renderer = await createCliRenderer({
    exitOnCtrlC: true,
    targetFps: 30,
})

const keyHandler = renderer.keyInput

keyHandler.on("keypress", (key: KeyEvent) => {
    console.log("Key name:", key.name)
    console.log("Input sequence:", key.sequence)
    console.log("Raw input:", key.raw)
    console.log("Ctrl pressed:", key.ctrl)
    console.log("Shift pressed:", key.shift)
    console.log("Alt pressed:", key.meta)
    console.log("Option pressed:", key.option)
})