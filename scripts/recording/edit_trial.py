#!/usr/bin/env python3
"""Build an editable EUI-Edits promo rough cut from a Snow Shot run.

The scene mapping and timing live in SCENE_SPEC. This script deliberately only
reads the recording run and writes into that run's edit-trial-v1 directory.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


WIDTH, HEIGHT, FPS = 1920, 1080, 30
APP_BOX = (240, 40, 1440, 900)  # x, y, width, height; preserve 16:10 capture.
FADE_S = 0.2
SAMPLE_RATE = 22050
OUT_DIR_NAME = "edit-trial-v1"

# Edit this mapping to match scene names emitted in report.json.
SCENE_SPEC = [
    {"key": "typing", "terms": ["typing", "type", "input", "write", "打字", "输入"]},
    {"key": "editing", "terms": ["editing", "edit", "markdown", "preview", "编辑", "渲染"]},
    {"key": "tabs", "terms": ["tabs", "tab", "switch", "文稿", "标签", "切换"]},
]
CARD_DURATIONS = {"opening": 4.0, "memory": 8.0, "closing": 3.0}
SUBTITLES = {
    "typing": ("输入 Markdown", "观察文字如何实时呈现"),
    "editing": ("边看边改", "在正文中继续整理思路"),
    "tabs": ("切换文稿", "接着阅读与编辑"),
}
FONT_CANDIDATES = [
    Path(r"C:\Windows\Fonts\msyh.ttc"),
    Path(r"C:\Windows\Fonts\msyhbd.ttc"),
    Path(r"C:\Windows\Fonts\simhei.ttf"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path, help="Recording run folder containing report.json and raw.mp4")
    parser.add_argument("--ffmpeg", type=Path, default=Path(r"D:\ruanjian\qwenasr\ffmpeg\ffmpeg.exe"), help="Path to ffmpeg.exe")
    return parser.parse_args()


def run_checked(args: list[str], *, capture: bool = False) -> str:
    try:
        result = subprocess.run(args, check=True, text=True, encoding="utf-8", errors="replace",
                                stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                                stderr=subprocess.PIPE if capture else subprocess.PIPE)
    except FileNotFoundError as exc:
        raise RuntimeError(f"找不到工具或素材：{args[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or "").strip()
        raise RuntimeError(f"命令失败（退出码 {exc.returncode}）：{args[0]}\n{detail[-3500:]}") from exc
    return result.stdout if capture else ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_inputs(run_dir: Path) -> tuple[dict, Path, Path, Path, dict]:
    run_dir = run_dir.resolve()
    report_path = run_dir / "report.json"
    raw_path = run_dir / "raw.mp4"
    if not report_path.is_file() or not raw_path.is_file():
        raise ValueError(f"--run 必须同时包含 report.json 和 raw.mp4：{run_dir}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    checks = report.get("checks")
    if not isinstance(checks, dict) or not checks or not all(value is True for value in checks.values()):
        raise ValueError("录制 report.json 的 checks 必须全部为 true")
    if report.get("demo_exit") != 0 or report.get("bridge_exit") != 0:
        raise ValueError("录制的 demo_exit 和 bridge_exit 必须均为 0")
    exe_value, recorded_hash = report.get("exe"), report.get("sha256")
    if not isinstance(exe_value, str) or not isinstance(recorded_hash, str):
        raise ValueError("report.json 必须包含 exe 路径和 sha256")
    exe_path = Path(exe_value)
    if not exe_path.is_file() or sha256(exe_path).casefold() != recorded_hash.casefold():
        raise ValueError(f"录制时 exe 哈希与当前文件不一致：{exe_path}")
    memory = report.get("memory_summary")
    if not isinstance(memory, dict):
        raise ValueError("report.json 缺少 memory_summary 对象")
    env = report.get("render_environment")
    if not isinstance(env, dict) or env.get("software_rendering") is not True:
        raise ValueError("本样片要求 report.json 明确记录 render_environment.software_rendering=true")
    if memory.get("process_count") != 1:
        raise ValueError("内存卡片要求单进程样本；memory_summary.process_count 必须为 1")
    for key in ("scenario", "pid", "samples", "private_ws_mib", "working_set_mib",
                "private_commit_mib", "document_bytes", "document_lines", "process_count"):
        if key not in memory:
            raise ValueError(f"memory_summary 缺少字段：{key}")
    scenes = report.get("scenes")
    if not isinstance(scenes, list) or not scenes:
        raise ValueError("report.json 缺少非空 scenes 数组")
    return report, report_path, raw_path, run_dir, memory


def match_scenes(scenes: list[dict]) -> list[dict]:
    selected = []
    used_names: set[str] = set()
    for spec in SCENE_SPEC:
        matches = []
        for scene in scenes:
            if not isinstance(scene, dict) or not isinstance(scene.get("name"), str):
                continue
            name = scene["name"].casefold()
            if any(term.casefold() in name for term in spec["terms"]):
                matches.append(scene)
        if len(matches) != 1:
            names = [s.get("name", "?") for s in scenes if isinstance(s, dict)]
            raise ValueError(f"镜头 {spec['key']} 匹配到 {len(matches)} 项；请按 SCENE_SPEC 调整名称关键字。当前 scenes：{names}")
        scene = matches[0]
        if scene["name"] in used_names:
            raise ValueError(f"同一素材镜头被重复匹配：{scene['name']}")
        used_names.add(scene["name"])
        try:
            start, end = float(scene["start_s"]), float(scene["end_s"])
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"镜头 {scene['name']} 缺少有效 start_s/end_s") from exc
        if not math.isfinite(start) or not math.isfinite(end) or start < 0 or end <= start:
            raise ValueError(f"镜头 {scene['name']} 时间范围无效：{start}–{end}")
        available = end - start
        target = available  # Use the complete recorded interval at 1x speed.
        selected.append({"key": spec["key"], "name": scene["name"], "source_start_s": start,
                         "source_end_s": end, "available_s": available, "duration_s": target,
                         "subtitle": list(SUBTITLES[spec["key"]]),
                         "source_truncated_s": 0.0, "duration_padded_s": 0.0})
    return selected


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    candidates = FONT_CANDIDATES[1:] + FONT_CANDIDATES[:1] if bold else FONT_CANDIDATES
    for path in candidates:
        if path.is_file():
            return ImageFont.truetype(str(path), size=size)
    raise RuntimeError("找不到支持简体中文的 Windows 字体（需要微软雅黑或黑体）")


def draw_center(draw: ImageDraw.ImageDraw, y: int, text: str, face: ImageFont.FreeTypeFont,
                fill: tuple[int, ...], *, max_width: int = 1500) -> int:
    lines: list[str] = []
    line = ""
    for char in text:
        candidate = line + char
        if line and draw.textbbox((0, 0), candidate, font=face)[2] > max_width:
            lines.append(line)
            line = char
        else:
            line = candidate
    if line:
        lines.append(line)
    total_height = sum(draw.textbbox((0, 0), part, font=face)[3] - draw.textbbox((0, 0), part, font=face)[1] for part in lines)
    current = y - total_height // 2
    for part in lines:
        bounds = draw.textbbox((0, 0), part, font=face)
        x = (WIDTH - (bounds[2] - bounds[0])) // 2
        draw.text((x, current - bounds[1]), part, font=face, fill=fill)
        current += bounds[3] - bounds[1] + 8
    return total_height


def make_card(path: Path, title: str, lines: list[str], *, label: str = "EUI-Edits") -> None:
    image = Image.new("RGB", (WIDTH, HEIGHT), (235, 242, 250))
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, WIDTH, 10), fill=(55, 126, 185))
    draw.rounded_rectangle((120, 120, WIDTH - 120, HEIGHT - 120), radius=28,
                           fill=(255, 255, 255), outline=(198, 213, 229), width=2)
    draw.text((180, 164), label, font=font(30, True), fill=(55, 104, 147))
    draw_center(draw, 365, title, font(64, True), (28, 47, 68), max_width=1500)
    if lines:
        line_font = font(34)
        gap = 62
        start_y = 600 - (len(lines) - 1) * gap // 2
        for index, line in enumerate(lines):
            draw_center(draw, start_y + index * gap, line, line_font, (69, 90, 113), max_width=1500)
    image.save(path)


def make_subtitle(path: Path, primary: str, secondary: str) -> None:
    image = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    x, y, w, h = APP_BOX[0], APP_BOX[1] + APP_BOX[3] + 15, APP_BOX[2], HEIGHT - (APP_BOX[1] + APP_BOX[3] + 15) - 22
    draw.rounded_rectangle((x, y, x + w, y + h), radius=16, fill=(248, 251, 255, 238), outline=(184, 203, 223, 248), width=2)
    draw_center(draw, y + 34, primary, font(30, True), (28, 47, 68), max_width=w - 70)
    draw_center(draw, y + 77, secondary, font(22), (69, 90, 113), max_width=w - 70)
    image.save(path)


def metric_number(value: object, label: str) -> float:
    if not isinstance(value, dict) or not isinstance(value.get("median"), (int, float)):
        raise ValueError(f"memory_summary.{label} 必须包含数值 median")
    return float(value["median"])


def metric_line(value: dict, label: str) -> str:
    median = metric_number(value, label)
    low, high = value.get("min"), value.get("max")
    if not isinstance(low, (int, float)) or not isinstance(high, (int, float)):
        raise ValueError(f"memory_summary.{label} 必须包含数值 min/max")
    mib_to_mb = 1.048576
    return f"{median * mib_to_mb:.1f} MB（范围 {float(low) * mib_to_mb:.1f}–{float(high) * mib_to_mb:.1f} MB）"


def make_music(path: Path, seconds: float) -> None:
    """Write a quiet original sine-pad placeholder with no voice or samples."""
    rate = SAMPLE_RATE
    total = round(seconds * rate)
    progression = [
        (261.63, 329.63, 392.00, 493.88),
        (220.00, 261.63, 329.63, 392.00),
        (174.61, 220.00, 261.63, 329.63),
        (196.00, 246.94, 293.66, 392.00),
    ]
    chunk_frames = 2048
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        base = 0
        while base < total:
            count = min(chunk_frames, total - base)
            pcm = array.array("h")
            for offset in range(count):
                index = base + offset
                t = index / rate
                chord = progression[int(t // 4) % len(progression)]
                fade_in = min(1.0, t / 1.5)
                fade_out = min(1.0, max(0.0, seconds - t) / 2.0)
                envelope = min(fade_in, fade_out) * (0.72 + 0.08 * math.sin(2 * math.pi * t / 9.0))
                pad = sum(math.sin(2 * math.pi * hz * t) for hz in chord) / len(chord)
                shimmer = math.sin(2 * math.pi * chord[2] * 2 * t) * 0.12
                sample = int(32767 * 0.055 * envelope * (pad * 0.88 + shimmer))
                pcm.extend((sample, sample))
            if sys.byteorder != "little":
                pcm.byteswap()
            wav.writeframesraw(pcm.tobytes())
            base += count


def ffprobe_info(ffprobe: Path, video: Path) -> dict:
    output = run_checked([str(ffprobe), "-v", "error", "-count_frames", "-show_entries",
                          "stream=codec_type,codec_name,width,height,avg_frame_rate,nb_read_frames,duration",
                          "-show_entries", "format=duration,size", "-of", "json", str(video)], capture=True)
    return json.loads(output)


def audio_peak_db(ffmpeg: Path, video: Path) -> float:
    result = subprocess.run([str(ffmpeg), "-hide_banner", "-i", str(video), "-map", "0:a:0",
                             "-af", "volumedetect", "-f", "null", os.devnull],
                            text=True, encoding="utf-8", errors="replace", stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(f"BGM 音量分析失败：{result.stderr[-2500:]}")
    match = re.search(r"max_volume:\s*(-?\d+(?:\.\d+)?) dB", result.stderr)
    if not match:
        raise RuntimeError("ffmpeg volumedetect 未返回 max_volume")
    return float(match.group(1))


def encode_card(ffmpeg: Path, png: Path, target: Path, duration: float) -> None:
    fade_out_at = max(0.0, duration - FADE_S)
    vf = f"fps={FPS},format=yuv420p,fade=t=in:st=0:d={FADE_S}:color=0xeff4fa,fade=t=out:st={fade_out_at}:d={FADE_S}:color=0xeff4fa"
    run_checked([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-loop", "1", "-framerate", str(FPS),
                 "-i", str(png), "-t", f"{duration:.6f}", "-vf", vf, "-an", "-c:v", "libx264", "-preset", "veryfast",
                 "-crf", "18", "-pix_fmt", "yuv420p", "-r", str(FPS), "-g", "60", "-keyint_min", "60",
                 "-sc_threshold", "0", "-movflags", "+faststart", str(target)])


def encode_scene(ffmpeg: Path, raw: Path, subtitle: Path, target: Path, scene: dict, available_s: float) -> None:
    duration = scene["duration_s"]
    source_duration = min(duration, available_s)
    pad = max(0.0, duration - source_duration)
    x, y, width, height = APP_BOX
    fade_out_at = max(0.0, duration - FADE_S)
    # GetWindowRect includes invisible DWM borders that expose the desktop.
    vf = (f"crop=iw-20:ih-12:10:0,scale={width}:{height}:force_original_aspect_ratio=decrease:flags=lanczos,"
          f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:color=0xeff4fa,setsar=1,fps={FPS},"
          f"tpad=stop_mode=clone:stop_duration={pad:.6f},trim=duration={duration:.6f},setpts=PTS-STARTPTS,"
          f"format=yuv420p[app];[app]pad={WIDTH}:{HEIGHT}:{x}:{y}:color=0xeff4fa[canvas];"
          f"[canvas][1:v]overlay=0:0:format=auto,format=yuv420p,"
          f"fade=t=in:st=0:d={FADE_S}:color=0xeff4fa,fade=t=out:st={fade_out_at:.6f}:d={FADE_S}:color=0xeff4fa[v]")
    run_checked([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{scene['source_start_s']:.6f}",
                 "-i", str(raw), "-loop", "1", "-framerate", str(FPS), "-i", str(subtitle), "-t", f"{duration:.6f}",
                 "-filter_complex", vf, "-map", "[v]", "-an", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
                 "-pix_fmt", "yuv420p", "-r", str(FPS), "-g", "60", "-keyint_min", "60", "-sc_threshold", "0",
                 "-movflags", "+faststart", str(target)])


def main() -> int:
    options = parse_args()
    ffmpeg = options.ffmpeg.expanduser().resolve()
    ffprobe = ffmpeg.with_name("ffprobe.exe" if ffmpeg.suffix.casefold() == ".exe" else "ffprobe")
    if not ffmpeg.is_file():
        raise ValueError(f"找不到 ffmpeg：{ffmpeg}")
    if not ffprobe.is_file():
        raise ValueError(f"ffprobe 必须和 ffmpeg 放在同一目录：{ffprobe}")
    report, report_path, raw, run_dir, memory = load_inputs(options.run)
    selected = match_scenes(report["scenes"])
    memory_source = memory["private_ws_mib"]
    memory_text = metric_line(memory_source, "private_ws_mib")
    # Validate the supporting measurements too, even though the card keeps its
    # focus on private working set.
    working_text = metric_line(memory["working_set_mib"], "working_set_mib")
    commit_text = metric_line(memory["private_commit_mib"], "private_commit_mib")
    samples = memory["samples"]
    if not isinstance(samples, (int, float, list)):
        raise ValueError("memory_summary.samples 必须是样本数量或样本数组")
    sample_count = len(samples) if isinstance(samples, list) else int(samples)
    if sample_count < 1:
        raise ValueError("memory_summary.samples 为空")
    total_s = CARD_DURATIONS["opening"] + sum(item["duration_s"] for item in selected) + CARD_DURATIONS["memory"] + CARD_DURATIONS["closing"]
    output_dir = run_dir / OUT_DIR_NAME
    output_dir.mkdir(parents=True, exist_ok=True)
    asset_dir = output_dir / "assets"
    asset_dir.mkdir(parents=True, exist_ok=True)

    opening = asset_dir / "opening.png"
    evidence = asset_dir / "memory-card.png"
    closing = asset_dir / "closing.png"
    make_card(opening, "轻量文本编辑器", ["常用 Markdown"], label="EUI-Edits")
    make_card(evidence, f"约 {metric_number(memory_source, 'private_ws_mib') * 1.048576:.1f} MB", [
        f"单进程专用工作集约 {metric_number(memory_source, 'private_ws_mib') * 1.048576:.1f} MB",
        f"提交大小约 {metric_number(memory['private_commit_mib'], 'private_commit_mib') * 1.048576:.1f} MB · 1 个进程",
        f"3 个小 MD 文稿 · {int(memory['document_bytes']):,} 字节 / {int(memory['document_lines']):,} 行 · {sample_count} 次采样",
        "软件渲染录屏条件 · 仅本次应用实测，不作跨软件性能结论",
    ], label="EUI-Edits · 测试记录")
    make_card(closing, "EUI-Edits", ["轻量文本编辑器 · 常用 Markdown"], label="感谢观看")
    subtitle_paths: dict[str, Path] = {}
    for scene in selected:
        path = asset_dir / f"subtitle-{scene['key']}.png"
        make_subtitle(path, *scene["subtitle"])
        subtitle_paths[scene["key"]] = path

    segments: list[tuple[str, Path, float]] = []
    for name, card, duration in (("opening", opening, CARD_DURATIONS["opening"]),):
        target = output_dir / f"segment-{name}.mp4"
        encode_card(ffmpeg, card, target, duration)
        segments.append((name, target, duration))
    for scene in selected:
        target = output_dir / f"segment-{scene['key']}.mp4"
        encode_scene(ffmpeg, raw, subtitle_paths[scene["key"]], target, scene, scene["available_s"])
        scene["subtitle_asset"] = str(subtitle_paths[scene["key"]].relative_to(output_dir))
        scene["duration_padded_s"] = max(0.0, scene["duration_s"] - min(scene["duration_s"], scene["available_s"]))
        segments.append((scene["key"], target, scene["duration_s"]))
    for name, card, duration in (("memory", evidence, CARD_DURATIONS["memory"]), ("closing", closing, CARD_DURATIONS["closing"])):
        target = output_dir / f"segment-{name}.mp4"
        encode_card(ffmpeg, card, target, duration)
        segments.append((name, target, duration))

    concat_list = output_dir / "segments.txt"
    concat_list.write_text("".join(f"file '{path.as_posix()}'\n" for _, path, _ in segments), encoding="utf-8")
    music = output_dir / "original-synth-placeholder.wav"
    make_music(music, total_s)
    trial = output_dir / "trial-v1.mp4"
    run_checked([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(concat_list),
                 "-i", str(music), "-filter:a", f"volume=0.7,afade=t=in:st=0:d=1.2,afade=t=out:st={max(0.0, total_s - 2.0):.3f}:d=2",
                 "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "aac", "-b:a", "96k", "-ar", str(SAMPLE_RATE),
                 "-t", f"{total_s:.6f}", "-movflags", "+faststart", str(trial)])

    probe = ffprobe_info(ffprobe, trial)
    streams = probe.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), {})
    audio = next((s for s in streams if s.get("codec_type") == "audio"), {})
    if (video.get("width"), video.get("height")) != (WIDTH, HEIGHT):
        raise RuntimeError(f"成片分辨率不符：{video.get('width')}x{video.get('height')}")
    if video.get("avg_frame_rate") != "30/1":
        raise RuntimeError(f"成片帧率不是 30fps：{video.get('avg_frame_rate')}")
    if not audio:
        raise RuntimeError("成片缺少合成 BGM 音轨")
    run_checked([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-i", str(trial), "-f", "null", os.devnull])
    measured_peak_db = audio_peak_db(ffmpeg, trial)

    # Deterministic one-frame-per-second contact sheet for the user to review.
    thumbs = []
    for second in range(int(math.ceil(total_s))):
        thumb = asset_dir / f"preview-{second:02d}.png"
        run_checked([str(ffmpeg), "-hide_banner", "-loglevel", "error", "-y", "-ss", f"{second + 0.5:.3f}",
                     "-i", str(trial), "-frames:v", "1", "-vf", "scale=480:270", str(thumb)])
        with Image.open(thumb) as frame:
            thumbs.append(frame.convert("RGB"))
    cols, thumb_w, thumb_h, label_h = 4, 480, 270, 34
    rows = math.ceil(len(thumbs) / cols)
    sheet = Image.new("RGB", (cols * thumb_w, rows * (thumb_h + label_h)), (17, 24, 39))
    draw = ImageDraw.Draw(sheet)
    label_font = font(20, True)
    for index, thumb in enumerate(thumbs):
        x = (index % cols) * thumb_w
        y = (index // cols) * (thumb_h + label_h)
        sheet.paste(thumb, (x, y))
        draw.text((x + 10, y + thumb_h + 5), f"{index + 0.5:.1f}s", font=label_font, fill=(224, 232, 245))
    contact_sheet = output_dir / "preview-contact-sheet.jpg"
    sheet.save(contact_sheet, quality=90)

    executable_path = Path(report["exe"])
    executable_sha = report["sha256"]
    evidence_ref = {
        "report_json": {"path": str(report_path), "sha256": sha256(report_path)},
        "raw_video": {"path": str(raw), "sha256": sha256(raw)},
        "executable": {"path": str(executable_path), "reported_sha256": executable_sha,
                       "current_sha256": sha256(executable_path), "matches_report": True},
        "memory_summary": memory,
        "render_environment": report["render_environment"],
    }
    storyboard = [
        "# EUI-Edits 宣传视频粗剪 v1",
        "",
        f"总时长：约 {total_s:.1f} 秒；1920×1080、30 fps。所有切点/时长可在 `edit_trial.py` 的 `SCENE_SPEC` 与 `CARD_DURATIONS` 修改。",
        "",
        "| 时间 | 画面 | 来源/说明 |",
        "|---|---|---|",
        f"| 0–4s | 开场卡：EUI-Edits / 轻量文本编辑器 / 常用 Markdown | 自动生成 |",
    ]
    cursor = CARD_DURATIONS["opening"]
    for scene in selected:
        end = cursor + scene["duration_s"]
        storyboard.append(f"| {cursor:.1f}–{end:.1f}s | {scene['key']}：{scene['subtitle'][0]} | raw.mp4 `{scene['name']}`，源区间 {scene['source_start_s']:.3f}–{scene['source_start_s'] + min(scene['duration_s'], scene['available_s']):.3f}s；底部字幕避开 app 画面 |")
        cursor = end
    storyboard.extend([
        f"| {cursor:.1f}–{cursor + CARD_DURATIONS['memory']:.1f}s | 内存卡：单进程专用工作集中位数 {memory_text}；3 个小 MD 文稿 | `memory_summary.private_ws_mib`；软件渲染录屏条件；仅本次应用实测，不作跨软件性能结论 |",
        f"| {total_s - CARD_DURATIONS['closing']:.1f}–{total_s:.1f}s | 收尾卡：EUI-Edits · 轻量文本编辑器 | 自动生成 |",
        "",
        "转场：镜头首尾各 0.2s 淡入淡出至浅色底，保留实录区间和 1 倍速；没有静帧延长。实录仅裁去左/右各 10px、底部 12px 的窗口外框，避免背景桌面进入画面。背景音乐：本脚本合成的低音量纯音和弦，无人声，供本版试听。",
        "",
        "## 可复核来源",
        "",
        f"- report.json SHA256：`{evidence_ref['report_json']['sha256']}`",
        f"- raw.mp4 SHA256：`{evidence_ref['raw_video']['sha256']}`",
        f"- 录制 exe SHA256：`{executable_sha}`（已和当前 exe 文件校验）",
        f"- 内存场景：`{memory['scenario']}`；PID `{memory['pid']}`；采样数 `{sample_count}`；专用工作集 `{memory_text}`；整体工作集 `{working_text}`；专用提交 `{commit_text}`；文档 `{int(memory['document_bytes'])} bytes / {int(memory['document_lines'])} lines`；进程数 `{memory['process_count']}`。MB 按 1 MiB = 1.048576 MB 换算。",
        "- `report.json` 的 `render_environment.software_rendering=true`；此项说明本次录屏测试采用软件渲染环境。",
        "- 内存口径：[微软 PROCESS_MEMORY_COUNTERS_EX2 字段定义](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex2)，[微软对任务管理器内存与提交的说明](https://blogs.windows.com/msedgedev/2021/01/13/investigate-microsoft-edge-memory-usage/)。工作集与提交是不同指标，本轮未测 CPU/GPU 负载。",
        "- 正式版建议：在默认渲染环境重测相同文稿；若对比浏览器架构应用，使用相同文稿、标签数量与采样时段，合计所属进程的专用工作集和私有提交，记录版本与插件条件，再配上过滤后的真实任务管理器画面。小文稿结果不能代表大文档。",
        "",
        "## 输出",
        "",
        "- `trial-v1.mp4`：粗剪样片；",
        "- `preview-contact-sheet.jpg`：每秒一帧的审片缩略图；",
        "- `edit-report.json`：输入哈希、指标来源、镜头裁切、编码与解码核验；",
        "- `assets/`：字幕/卡片图，可在 Pillow 生成函数中修改；",
        "- `original-synth-placeholder.wav`：试听占位 BGM；",
        "- `segments.txt` 与 `segment-*.mp4`：可重排的分镜清单与单镜头中间文件。",
    ])
    storyboard_path = output_dir / "storyboard.md"
    storyboard_path.write_text("\n".join(storyboard) + "\n", encoding="utf-8")
    edit_report = {
        "status": "complete",
        "output": {"path": str(trial), "sha256": sha256(trial), "bytes": trial.stat().st_size,
                   "duration_s": float(video.get("duration", probe.get("format", {}).get("duration", 0))),
                   "width": video.get("width"), "height": video.get("height"), "avg_frame_rate": video.get("avg_frame_rate"),
                   "decoded_frames": video.get("nb_read_frames"), "audio_codec": audio.get("codec_name")},
        "verification": {"ffprobe": "passed", "full_decode": "passed", "expected_duration_s": total_s,
                         "source_crop_pixels": {"left": 10, "right": 10, "top": 0, "bottom": 12},
                         "transition": "0.2s fade to light at segment boundaries", "audio_max_volume_db": measured_peak_db,
                         "music": "original sine-pad placeholder; no voice; low mix gain"},
        "inputs": evidence_ref,
        "tooling": {"ffmpeg": str(ffmpeg), "ffmpeg_sha256": sha256(ffmpeg), "ffprobe": str(ffprobe),
                    "ffprobe_sha256": sha256(ffprobe), "raw_input": "raw.mp4", "report_input": "report.json"},
        "scene_spec": selected,
        "metrics_shown": {"private_ws_mib": memory_source,
                          "private_ws_mb_decimal": metric_number(memory_source, "private_ws_mib") * 1.048576,
                          "working_set_mib": memory["working_set_mib"],
                          "working_set_mb_decimal": metric_number(memory["working_set_mib"], "working_set_mib") * 1.048576,
                          "private_commit_mib": memory["private_commit_mib"],
                          "private_commit_mb_decimal": metric_number(memory["private_commit_mib"], "private_commit_mib") * 1.048576,
                          "scenario": memory["scenario"], "samples": sample_count,
                          "document_bytes": memory["document_bytes"], "document_lines": memory["document_lines"],
                          "process_count": memory["process_count"], "software_rendering": report["render_environment"]["software_rendering"]},
        "deliverables": {"storyboard": str(storyboard_path), "contact_sheet": str(contact_sheet),
                         "music_wav": str(music), "report": str(output_dir / "edit-report.json")},
    }
    report_out = output_dir / "edit-report.json"
    report_out.write_text(json.dumps(edit_report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(trial), "duration_s": edit_report["output"]["duration_s"],
                      "contact_sheet": str(contact_sheet), "report": str(report_out)}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, json.JSONDecodeError) as error:
        print(f"edit_trial: {error}", file=sys.stderr)
        raise SystemExit(2)
