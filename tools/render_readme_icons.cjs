#!/usr/bin/env node
"use strict";

const { execFileSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const output = path.resolve(process.argv[2] || path.join(root, "docs/screenshots/icons-overview.png"));
const width = 2400;
const height = 1280;
const families = [
  { label: "Markdown", kind: "md", accent: "#7962ce", tint: "#f2eefb", example: ".md  ·  .markdown" },
  { label: "Text", kind: "txt", accent: "#397bd1", tint: "#edf4fc", example: ".txt  ·  .log" },
  { label: "Code", kind: "code", accent: "#168879", tint: "#edf7f4", example: ".py  ·  .cpp" },
  { label: "Data", kind: "data", accent: "#d47b25", tint: "#fbf3e9", example: ".json  ·  .yaml" },
  { label: "File", kind: "file", accent: "#707a88", tint: "#f0f2f5", example: ".gitignore  ·  LICENSE" },
];

let sharp;
try {
  sharp = require("sharp");
} catch {
  sharp = require("C:/Users/123/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/sharp");
}

function xml(value) {
  return value.replace(/[&<>"']/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&apos;",
  })[character]);
}

function svg(body, w = width, h = height) {
  return Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">${body}</svg>`);
}

function text(x, y, content, size, color = "#263044", extra = "") {
  return `<text x="${x}" y="${y}" font-family="Segoe UI, sans-serif" font-size="${size}" fill="${color}" ${extra}>${xml(content)}</text>`;
}

function background() {
  const cards = families.map((family, index) => {
    const x = 72 + index * 456;
    const center = x + 216;
    return `
      <rect x="${x}" y="734" width="432" height="456" rx="28" fill="#dce1ea" opacity=".28"/>
      <rect x="${x}" y="726" width="432" height="456" rx="28" fill="url(#card${index})" stroke="#e0e4ed" stroke-width="2"/>
      <rect x="${x + 24}" y="750" width="62" height="6" rx="3" fill="${family.accent}" opacity=".75"/>
      ${text(x + 389, 782, String(index + 1).padStart(2, "0"), 28, family.accent, 'text-anchor="end" opacity=".34"')}
      ${text(center, 1094, family.label, 40, "#303b4f", 'text-anchor="middle" font-weight="600"')}
      ${text(center, 1151, family.example, 29, "#778295", 'text-anchor="middle"')}
    `;
  }).join("");
  const defs = families.map((family, index) => `
    <linearGradient id="card${index}" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="${family.tint}"/>
      <stop offset=".8" stop-color="#ffffff"/>
      <stop offset="1" stop-color="#ffffff"/>
    </linearGradient>`).join("");
  const dots = families.map((family, index) => `<circle cx="${563 + index * 40}" cy="464" r="8" fill="${family.accent}"/>`).join("");
  return svg(`
    <defs>
      ${defs}
      <linearGradient id="page" x1="0" y1="0" x2="1" y2="1">
        <stop offset="0" stop-color="#edeafb"/>
        <stop offset=".5" stop-color="#f8f9fc"/>
        <stop offset="1" stop-color="#edf2f8"/>
      </linearGradient>
      <radialGradient id="glow">
        <stop offset="0" stop-color="#d5cbee" stop-opacity=".58"/>
        <stop offset="1" stop-color="#d5cbee" stop-opacity="0"/>
      </radialGradient>
    </defs>
    <rect width="2400" height="1280" fill="url(#page)"/>
    <ellipse cx="286" cy="327" rx="326" ry="301" fill="url(#glow)"/>
    <path d="M-100 534C350 550 380 20 1010-80" fill="none" stroke="#d7d0ed" stroke-width="2" opacity=".55"/>
    <path d="M-70 564C425 565 475 24 1100-60" fill="none" stroke="#ddd7ef" stroke-width="2" opacity=".45"/>
    <path d="M1598-80L2450 735M1698-80L2450 635" fill="none" stroke="#dfe5f0" stroke-width="2" opacity=".55"/>
    ${text(76, 79, "EUI-EDITS / ICON FAMILY", 25, "#8b81a5", 'letter-spacing="3" font-weight="600"')}
    ${text(550, 184, "APPLICATION ICON", 27, "#8b7ab3", 'letter-spacing="3" font-weight="600"')}
    ${text(545, 281, "EUI-Edits", 92, "#293046", 'font-weight="600" letter-spacing="-2"')}
    ${text(551, 348, "A matching icon family", 38, "#68748a")}
    <rect x="550" y="383" width="327" height="46" rx="23" fill="#ffffff" opacity=".7"/>
    ${text(571, 415, "Native Windows editor", 27, "#79658e")}
    ${dots}
    ${text(1310, 80, "SMALL ICONS IN USE", 26, "#7c86a0", 'letter-spacing="2" font-weight="600"')}
    <path d="M72 620H2328" stroke="#dbe1ec" stroke-width="2"/>
    ${text(72, 689, "Document icons", 46, "#2d374e", 'font-weight="600"')}
    ${text(2328, 686, "Full-size artwork", 29, "#7d879a", 'text-anchor="end"')}
    ${cards}
    ${text(72, 1240, "Five document families, from the desktop icon to the file list.", 27, "#808a9b")}
    <path d="M2200 1230H2328" stroke="#c9c1e0" stroke-width="3"/>
  `);
}

async function rounded(input, radius) {
  const meta = await sharp(input).metadata();
  const mask = svg(`<rect width="${meta.width}" height="${meta.height}" rx="${radius}" fill="white"/>`, meta.width, meta.height);
  return sharp(input).ensureAlpha().composite([{ input: mask, blend: "dest-in" }]).png().toBuffer();
}

async function screenshotCard(source, { contentWidth, angle = 0, opacity = 1 }) {
  // Both images are existing real-window README screenshots. Crop only the
  // five actual library rows, retaining the application's own 16-DIP glyphs.
  const rows = await sharp(source)
    .extract({ left: 8, top: 222, width: 307, height: 166 })
    .resize(contentWidth, Math.round(contentWidth * 166 / 307), { kernel: "lanczos3" })
    .png().toBuffer();
  const meta = await sharp(rows).metadata();
  const w = meta.width + 40;
  const h = meta.height + 40;
  const card = await sharp(svg(`<rect width="${w}" height="${h}" rx="26" fill="#ffffff"/>`, w, h))
    .composite([{ input: await rounded(rows, 14), left: 20, top: 20 }])
    .png().toBuffer();
  let result = await rounded(card, 26);
  if (angle) result = await sharp(result).rotate(angle, { background: "#00000000" }).png().toBuffer();
  if (opacity !== 1) {
    result = await sharp(result).ensureAlpha().linear([1, 1, 1, opacity], [0, 0, 0, 0]).png().toBuffer();
  }
  return result;
}

async function shadow(input, opacity = .13, sigma = 14) {
  const meta = await sharp(input).metadata();
  const pad = Math.ceil(sigma * 3);
  const silhouette = await sharp(input).ensureAlpha().linear([0, 0, 0, opacity], [68, 53, 107, 0]).png().toBuffer();
  const result = await sharp({ create: { width: meta.width + pad * 2, height: meta.height + pad * 2, channels: 4, background: "#00000000" } })
    .composite([{ input: silhouette, left: pad, top: pad }]).blur(sigma).png().toBuffer();
  return { input: result, pad };
}

function pythonCommand() {
  if (process.env.NEOEDITOR_PYTHON) return process.env.NEOEDITOR_PYTHON;
  const preferred = "D:/Python312/python.exe";
  return fs.existsSync(preferred) ? preferred : "python";
}

async function main() {
  const tempRoot = path.resolve(os.tmpdir());
  const tempDir = fs.mkdtempSync(path.join(tempRoot, "neoeditor-readme-icons-"));
  try {
    execFileSync(pythonCommand(), [path.join(root, "tools/render_readme_document_icons.py"), "--output-dir", tempDir], { stdio: "inherit" });
    const overlays = [];
    const darkCard = await screenshotCard(path.join(root, "docs/screenshots/code-dark.png"), { contentWidth: 650, angle: 4, opacity: .55 });
    const lightCard = await screenshotCard(path.join(root, "docs/screenshots/markdown-light.png"), { contentWidth: 790, angle: -2 });
    for (const [input, left, top] of [[darkCard, 1590, 103], [lightCard, 1290, 172]]) {
      const cast = await shadow(input);
      overlays.push({ input: cast.input, left: left - cast.pad, top: top - cast.pad + 12 });
      overlays.push({ input, left, top });
    }
    const app = await sharp(path.join(root, "assets/icon.svg"), { density: 192 }).resize(350, 350).png().toBuffer();
    const appShadow = await shadow(app, .16, 16);
    overlays.push({ input: appShadow.input, left: 145 - appShadow.pad, top: 172 - appShadow.pad + 16 });
    overlays.push({ input: app, left: 145, top: 172 });
    for (let index = 0; index < families.length; index++) {
      const full = await sharp(path.join(tempDir, `${families[index].kind}-256.png`)).resize(240, 240).png().toBuffer();
      overlays.push({ input: full, left: 72 + index * 456 + 96, top: 804 });
    }
    fs.mkdirSync(path.dirname(output), { recursive: true });
    await sharp(background()).composite(overlays).png({ compressionLevel: 9 }).toFile(output);
    const meta = await sharp(output).metadata();
    console.log(`Rendered ${output} (${meta.width}x${meta.height}, ${fs.statSync(output).size} bytes)`);
  } finally {
    const cleanupTarget = path.resolve(tempDir);
    if (path.dirname(cleanupTarget) !== tempRoot || !path.basename(cleanupTarget).startsWith("neoeditor-readme-icons-")) {
      throw new Error(`Refusing to clean unexpected temporary path: ${cleanupTarget}`);
    }
    fs.rmSync(cleanupTarget, { recursive: true, force: true });
  }
}

main().catch(error => { console.error(error); process.exitCode = 1; });
