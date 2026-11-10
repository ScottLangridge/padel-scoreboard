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
    return CLIPS.map((clip, i) => {
        clipIdx = i;
        const frameCount = Math.ceil(clip.dur * ${FPS} - 1e-9) + 1;   // one frame past the end, like the page
        const frames = [];
        for (let frame = 0; frame < frameCount; frame++) {
            animate(frame, frame / ${FPS});
            let hex = "";
            for (let i_nibble = 0; i_nibble < 128; i_nibble++) {
                let nibble = 0;
                for (let i_bit = 0; i_bit < 4; i_bit++) nibble = nibble * 2 + (buf[i_nibble * 4 + i_bit] > 0.5 ? 1 : 0);
                hex += nibble.toString(16);
            }
            frames.push(hex);
        }
        return { name: clip.name, dur: clip.dur, loopFrom: clip.loopFrom ?? null, frames };
    });`;
const rendered = new Function(html.slice(start, end) + "\n" + driver)();

for (const clip of rendered) {
    const merged = [];
    for (const hex of clip.frames) {
        if (merged.length && merged[merged.length - 1].hex === hex) {
            merged[merged.length - 1].count++;
        } else {
            merged.push({ hex, count: 1 });
        }
    }
    const json = {
        name: clip.name,
        width: 32,
        height: 16,
        fps_source: FPS,
        duration_s: Math.round(clip.dur * 100) / 100,
        loop_from_s: clip.loopFrom,
        encoding: "hex: 128 hex chars = 512 bits, row-major from top-left, MSB first; 1 = LED on",
        frames: merged.map(run => ({ ms: Math.round(run.count * 1000 / FPS), hex: run.hex })),
    };
    const file = path.join(outDir, NAMES[clip.name] + ".json");
    fs.writeFileSync(file, JSON.stringify(json));
    console.log(`${file}: ${clip.frames.length} frames, ${merged.length} merged`);
}
