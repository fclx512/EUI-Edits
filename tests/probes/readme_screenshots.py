"""Capture real EUI-Edits README views using isolated sample files and settings."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

import win_capture as cad
from capture_markdown import window_for_pid
from text_files import registry_snapshot


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--evidence', required=True)
    parser.add_argument('--library', help='Optional directory for the sample documents; existing different content is never overwritten.')
    args = parser.parse_args()
    exe, output, evidence = (Path(p).resolve() for p in [args.exe, args.out, args.evidence])
    output.mkdir(parents=True, exist_ok=True)
    evidence.mkdir(parents=True, exist_ok=True)
    cad.make_dpi_aware()
    cad.assert_unlocked('README screenshots')
    cad.assert_no_foreign_instance('README screenshots')
    original_registry = registry_snapshot()
    library = Path(args.library).resolve() if args.library else evidence / '示例文档'
    library.mkdir(parents=True, exist_ok=True)
    markdown = '''# 把想法写下来

在同一份文档中写作、整理与阅读。**重点更清晰**，`代码` 也有自己的样式。

## 今日清单
- [x] 整理一份简洁的笔记
- [ ] 记录下一步的想法

> 简单的工具，让注意力留在内容上。

## 资料一览
| 文件 | 用途 |
| --- | --- |
| Markdown | 写作与笔记 |
| JSON / Python | 查看与修改 |

## 一小段配置
```json
{"theme": "light", "language": "zh-CN", "word_wrap": true}
```
'''
    config = {
        'name': 'EUI-Edits',
        'version': '0.1.0',
        'interface': {'theme': 'dark', 'language': 'en', 'word_wrap': True, 'font_size': 18},
        'documents': [
            {'name': 'notes.md', 'type': 'markdown'},
            {'name': 'settings.json', 'type': 'configuration'},
            {'name': 'hello.py', 'type': 'code'},
        ],
        'library': {'show_all_files': True, 'load_document_on_open': True},
    }
    contents = {
        '写作示例.md': markdown,
        'settings.json': json.dumps(config, ensure_ascii=False, indent=2) + '\n',
        'hello.py': 'from pathlib import Path\n\n\ndef read_note(path: Path) -> str:\n    return path.read_text(encoding="utf-8")\n\n\nif __name__ == "__main__":\n    print(read_note(Path("notes.md")))\n',
        '随手记录.txt': '把短暂的灵感留下来。\n\n写一份笔记，或者修改一个文本文件。\n',
        '.gitignore': 'build/\n.cache/\n*.tmp\n',
    }
    for name, text in contents.items():
        sample = library / name
        if sample.exists():
            assert sample.read_text(encoding='utf-8') == text, f'Existing sample is different: {sample}'
        else:
            sample.write_text(text, encoding='utf-8')
    sample_hashes = {name: digest(library / name) for name in contents}
    report = {'exe': str(exe), 'exe_sha256': digest(exe), 'version': '0.1.0',
              'system_dpi': None, 'scale': 1, 'screenshots': [], 'exits': []}

    for view, language, theme, filename in [
        ('markdown-light', 'zh-CN', 1, '写作示例.md'),
        ('code-dark', 'en', 0, 'settings.json'),
        ('settings-en', 'en', 1, '写作示例.md'),
        ('associations-zh', 'zh-CN', 1, '写作示例.md'),
    ]:
        runtime = evidence / view
        settings = runtime / 'appdata/EUI-Edits/settings.ini'
        settings.parent.mkdir(parents=True, exist_ok=True)
        temp = runtime / 'temp'
        temp.mkdir(exist_ok=True)
        doc = library / filename
        values = {'vault': str(library), 'last_file': str(doc), 'mode': 1, 'theme': theme,
                  'ui_language': language, 'ui_scale': 1, 'ui_font_size': 14,
                  'editor_font_size': 18, 'vault_width': 260, 'line_numbers': int(view == 'code-dark'),
                  'readable_width': 0, 'show_status_bar': 1, 'animations': 0}
        settings.write_text(''.join(f'{k}={v}\n' for k, v in values.items()), encoding='utf-8')
        env = dict(os.environ, APPDATA=str(settings.parent.parent), TEMP=str(temp), TMP=str(temp),
                   NEO_SINGLE_INSTANCE='0', NEO_D2D_SOFTWARE='1', NEO_WIN32_DC='1')
        proc = subprocess.Popen([str(exe), str(doc)], cwd=exe.parent, env=env)
        hwnd = None

        def owned():
            cad.assert_unlocked(view)
            owner = ctypes.c_ulong()
            cad.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            alive = proc.poll() is None
            active = alive and owner.value == proc.pid and cad.ensure_foreground(hwnd)
            assert active, f'window ownership/foreground failed: hwnd={hwnd}, owner={owner.value}, pid={proc.pid}, exit={proc.poll()}, foreground={cad.user32.GetForegroundWindow()}'

        def key(*keys):
            owned()
            for code in keys:
                cad.user32.keybd_event(code, 0, 0, 0)
                time.sleep(.03)
            for code in reversed(keys):
                cad.user32.keybd_event(code, 0, 2, 0)
            time.sleep(.5)

        try:
            for _ in range(150):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            assert hwnd, 'owned window did not appear'
            owned()
            height = 1120 if view in ('settings-en', 'associations-zh') else 980
            cad.user32.SetWindowPos(hwnd, None, 30, 30, 1480, height, 4)
            time.sleep(1.2)
            unit = cad.user32.GetDpiForWindow(hwnd) / 96
            report['system_dpi'] = cad.user32.GetDpiForWindow(hwnd)
            key(0x1B)
            if view in ('settings-en', 'associations-zh'):
                key(0x11, 0xBC)
                if view == 'associations-zh':
                    cad.click(hwnd, round(85 * unit), round(208 * unit))
            time.sleep(.8)
            owned()
            width, height, pixels = cad.capture_client(hwnd)
            cad.user32.WindowFromPoint.argtypes = [cad.wintypes.POINT]
            cad.user32.WindowFromPoint.restype = ctypes.c_void_p
            cad.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
            cad.user32.GetAncestor.restype = ctypes.c_void_p
            for x in [16, width // 4, width // 2, 3 * width // 4, width - 16]:
                for y in [16, height // 4, height // 2, 3 * height // 4, height - 16]:
                    point = cad.wintypes.POINT(x, y)
                    cad.user32.ClientToScreen(hwnd, ctypes.byref(point))
                    assert cad.user32.GetAncestor(cad.user32.WindowFromPoint(point), 2) == hwnd, 'window is obscured'
            image = output / (view + '.png')
            cad.write_png(str(image), width, height, pixels)
            report['screenshots'].append({'file': image.name, 'sha256': digest(image),
                                          'width': width, 'height': height, 'language': language,
                                          'theme': 'light' if theme else 'dark', 'pid': proc.pid,
                                          'foreground_owned': True, 'occlusion_points_checked': 25})
            key(0x1B)
            cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
            proc.wait(timeout=10)
            assert proc.returncode == 0
            report['exits'].append({'view': view, 'code': proc.returncode})
            print(f'{view}: {width}x{height}, normal exit', flush=True)
        finally:
            if proc.poll() is None and hwnd:
                cad.user32.PostMessageW(hwnd, 0x10, 0, 0)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    report['forced_cleanup'] = True
                    proc.terminate()
                    proc.wait()
            (evidence / 'conditions.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    assert sample_hashes == {name: digest(library / name) for name in contents}, 'sample documents changed'
    assert original_registry == registry_snapshot(), 'production associations changed'
    assert not report.get('forced_cleanup')
    report['sample_files_unchanged'] = True
    report['production_associations_unchanged'] = True
    (evidence / 'conditions.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
