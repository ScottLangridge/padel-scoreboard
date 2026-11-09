#!/usr/bin/env node
// Renders the reference frames (design/frames/<clip>.json) from the animation
// code in design/p10-sim.html, so the simulator stays the source of truth:
//
//     node tools/render_reference.mjs design/p10-sim.html design/frames
//
// Runs the script section of the page up to its canvas rendering code, then
// calls animate() at 30 fps for every clip exactly as the page would, and
// writes {ms, hex} frames with identical consecutive frames merged.

import fs from "node:fs";
import path from "node:path";

const [, , simPath, outDir] = process.argv;
if (!simPath || !outDir) {
    console.error("usage: render_reference.mjs <p10-sim.html> <frames dir>");
    process.exit(2);
}

const FPS = 30;
const NAMES = {
    Splash: "splash", Bind: "bind", "Serve?": "serve", "Point L": "point-l", "Point R": "point-r",
    Undo: "undo", Game: "game", Set: "set", Match: "match", Swap: "swap", Golden: "golden",
};

const html = fs.readFileSync(simPath, "utf8");
const start = html.indexOf("<script>") + "<script>".length;
const end = html.indexOf("// Rendering", start);
if (start < "<script>".length || end < 0) {
    throw new Error("could not find the script section");
}
const driver = `
    return CLIPS.map((c, i) => {
        clipIdx = i;
        const n = Math.ceil(c.dur * ${FPS} - 1e-9) + 1;   // one frame past the end, like the page
        const frames = [];
        for (let f = 0; f < n; f++) {
            animate(f, f / ${FPS});
            let hex = "";
            for (let k = 0; k < 128; k++) {
                let v = 0;
                for (let b = 0; b < 4; b++) v = v * 2 + (buf[k * 4 + b] > 0.5 ? 1 : 0);
                hex += v.toString(16);
            }
            frames.push(hex);
        }
        return { name: c.name, dur: c.dur, loopFrom: c.loopFrom ?? null, frames };
    });`;
const clips = new Function(html.slice(start, end) + "\n" + driver)();

for (const c of clips) {
    const merged = [];
    for (const hex of c.frames) {
        if (merged.length && merged[merged.length - 1].hex === hex) {
            merged[merged.length - 1].n++;
        } else {
            merged.push({ hex, n: 1 });
        }
    }
    const json = {
        name: c.name,
        width: 32,
        height: 16,
        fps_source: FPS,
        duration_s: Math.round(c.dur * 100) / 100,
        loop_from_s: c.loopFrom,
        encoding: "hex: 128 hex chars = 512 bits, row-major from top-left, MSB first; 1 = LED on",
        frames: merged.map(m => ({ ms: Math.round(m.n * 1000 / FPS), hex: m.hex })),
    };
    const file = path.join(outDir, NAMES[c.name] + ".json");
    fs.writeFileSync(file, JSON.stringify(json));
    console.log(`${file}: ${c.frames.length} frames, ${merged.length} merged`);
}
