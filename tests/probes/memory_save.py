"""Same-process save memory trace for EUI-Edits. Does not trim or touch foreign processes."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
from ctypes import wintypes

import win_capture as cad
from capture_markdown import window_for_pid, write_settings


class Counters(ctypes.Structure):
    # PROCESS_MEMORY_COUNTERS_EX2: private commit, private working set, shared commit.
    _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD)] + [
        (name, ctypes.c_size_t) for name in (
            "peak_ws", "ws", "peak_paged", "paged", "peak_nonpaged", "nonpaged",
            "pagefile", "peak_pagefile", "private", "private_ws", "shared_commit")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-kib", type=int, default=64)
    ap.add_argument("--extended", action='store_true', help='verify Unicode save, overwrite and helper failure')
    args = ap.parse_args()
    exe, out = Path(args.exe).resolve(), Path(args.out).resolve()
    if not exe.is_file():
        raise FileNotFoundError(exe)
    if args.size_kib < 1 or args.size_kib > 65536:
        raise ValueError("--size-kib must be 1..65536")
    cad.make_dpi_aware()
    cad.assert_unlocked("save memory probe")
    cad.assert_no_foreign_instance("save memory probe")
    out.mkdir(parents=True, exist_ok=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    user.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    user.EnumChildWindows.argtypes = [wintypes.HWND, ctypes.c_void_p, ctypes.c_void_p]
    user.GetDlgCtrlID.argtypes = [wintypes.HWND]
    user.GetDlgCtrlID.restype = ctypes.c_int
    user.SetDlgItemTextW.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.LPCWSTR]
    user.SetDlgItemTextW.restype = wintypes.BOOL
    user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user.IsWindowVisible.argtypes = [wintypes.HWND]
    user.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user.GetForegroundWindow.restype = wintypes.HWND

    samples, stages = [], []
    stop = threading.Event()
    # Keep fixtures and partial traces reviewable even if a modal check fails.
    with __import__('contextlib').nullcontext(str(out / 'runtime')) as temp:
        root = Path(temp)
        root.mkdir(parents=True, exist_ok=False)
        appdata = root / "appdata"
        appdata.mkdir()
        doc = root / "existing.md"
        # Repeated Markdown at 64 KiB mirrors the reported small-document scenario.
        block = "# 标题\n\n正文 text **bold** [link](https://example.invalid)。\n\n- item\n\n"
        content = (block * ((args.size_kib * 1024 // len(block.encode("utf-8"))) + 1))[:]
        doc.write_text(content, encoding="utf-8")
        initial_document_bytes=doc.stat().st_size
        write_settings(str(appdata / "EUI-Edits" / "settings.ini"), str(doc), 0, 16, "1")
        env = dict(os.environ, APPDATA=str(appdata), NEO_D2D_SOFTWARE="1", NEO_LIVE_RESIZE="0")
        proc = subprocess.Popen([str(exe), str(doc)], cwd=exe.parent, env=env)
        handle = kernel.OpenProcess(0x0410, False, proc.pid)
        if not handle:
            raise OSError(ctypes.get_last_error(), "OpenProcess failed")
        started = time.monotonic()
        def sample():
            c = Counters(); c.cb = ctypes.sizeof(c)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(c), c.cb):
                return
            samples.append(dict(seconds=round(time.monotonic() - started, 3),
                                ws_mib=c.ws / 1048576,
                                private_ws_mib=c.private_ws / 1048576,
                                private_commit_mib=c.private / 1048576,
                                peak_ws_mib=c.peak_ws / 1048576,
                                peak_private_commit_mib=c.peak_pagefile / 1048576))
        hwnd = None
        try:
            for _ in range(200):
                hwnd = window_for_pid(proc.pid)
                if hwnd or proc.poll() is not None:
                    break
                time.sleep(.1)
            if not hwnd:
                raise RuntimeError("owned main window not found")
            cad.assert_unlocked("save memory probe")
            owner = wintypes.DWORD()
            user.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value != proc.pid or not cad.ensure_foreground(hwnd):
                raise RuntimeError("main window is not owned and foreground")
            def sampler():
                while not stop.is_set():
                    sample(); stop.wait(.05)
            thread = threading.Thread(target=sampler, daemon=True)
            thread.start()
            def owned():
                cad.assert_unlocked("save memory probe")
                if proc.poll() is not None:
                    raise RuntimeError("probe process exited")
                pid = wintypes.DWORD()
                user.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                if pid.value != proc.pid or not cad.ensure_foreground(hwnd):
                    raise RuntimeError("owned foreground lost")
            def stage(name, seconds=2):
                owned(); time.sleep(seconds); sample()
                stages.append(dict(name=name, **samples[-1]))
                print(name, {k:round(v,2) if isinstance(v,float) else v for k,v in samples[-1].items()}, flush=True)
            def key(*keys):
                owned()
                for k in keys:
                    user.keybd_event(k, user.MapVirtualKeyW(k, 0), 0, 0); time.sleep(.04)
                for k in reversed(keys):
                    user.keybd_event(k, user.MapVirtualKeyW(k, 0), 2, 0); time.sleep(.04)
                time.sleep(.3)
            def ctrl_s(): key(0x11, 0x53)
            def save_dialog():
                # GetSaveFileNameW opens the in-process #32770 dialog. Set its real filename field.
                dialog = None
                owned_pids = {proc.pid, *cad.direct_child_process_ids(proc.pid)}
                def enum_proc(candidate, _):
                    nonlocal dialog
                    pid = wintypes.DWORD(); user.GetWindowThreadProcessId(candidate, ctypes.byref(pid))
                    if pid.value in owned_pids and user.IsWindowVisible(candidate):
                        cls = ctypes.create_unicode_buffer(64); user.GetClassNameW(candidate, cls, 64)
                        if cls.value == "#32770": dialog = candidate; return False
                    return True
                callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(enum_proc)
                for _ in range(50):
                    owned_pids = {proc.pid, *cad.direct_child_process_ids(proc.pid)}
                    user.EnumWindows(callback, 0)
                    if dialog: break
                    time.sleep(.1)
                if not dialog:
                    raise RuntimeError("owned Save As dialog not found")
                pid = wintypes.DWORD(); user.GetWindowThreadProcessId(dialog, ctypes.byref(pid))
                if pid.value not in owned_pids:
                    raise RuntimeError("save dialog PID mismatch")
                return dialog
            def cancel_save_dialog():
                dialog = save_dialog()
                if user.GetForegroundWindow() != dialog:
                    raise RuntimeError("Save As dialog is not foreground")
                user.keybd_event(0x1B, 0, 0, 0); user.keybd_event(0x1B, 0, 2, 0)
                time.sleep(.5)
            def set_save_dialog(path, overwrite=False):
                dialog = save_dialog()
                # Explorer-style dialogs nest the filename Edit inside a ComboBox.
                edits = []
                @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                def edit_visit(child, _):
                    cls = ctypes.create_unicode_buffer(64)
                    user.GetClassNameW(child, cls, 64)
                    if cls.value == 'Edit' and user.IsWindowVisible(child):
                        edits.append(child)
                    return True
                user.EnumChildWindows(dialog, edit_visit, 0)
                if len(edits) != 1:
                    raise RuntimeError(f"expected one visible filename Edit, got {len(edits)}")
                user.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
                filename_buffer = ctypes.create_unicode_buffer(str(path))
                user.SendMessageW(edits[0], 0x000C, 0, ctypes.cast(filename_buffer, ctypes.c_void_p).value)
                user.keybd_event(0x0D, 0, 0, 0); user.keybd_event(0x0D, 0, 2, 0)
                time.sleep(1)
                if overwrite:
                    # The fixture is the only target; accept the native overwrite prompt.
                    foreground=user.GetForegroundWindow();pid=wintypes.DWORD()
                    user.GetWindowThreadProcessId(foreground,ctypes.byref(pid))
                    if pid.value not in cad.direct_child_process_ids(proc.pid):
                        raise RuntimeError('owned helper overwrite prompt absent')
                    user.GetDlgItem.argtypes=[wintypes.HWND,ctypes.c_int];user.GetDlgItem.restype=wintypes.HWND
                    actions=[]; controls=[]
                    @ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
                    def action_visit(child,_):
                        cls=ctypes.create_unicode_buffer(80);label=ctypes.create_unicode_buffer(256)
                        user.GetClassNameW(child,cls,80);user.GetWindowTextW(child,label,256)
                        cid=user.GetDlgCtrlID(child);controls.append(dict(cls=cls.value,label=label.value,id=cid))
                        if cls.value=='Button' and (cid==6 or label.value.startswith(('是','Yes'))):actions.append(child)
                        return True
                    user.GetWindowTextW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]
                    user.EnumChildWindows(foreground,action_visit,0)
                    (out/'overwrite-controls.json').write_text(json.dumps(controls,ensure_ascii=False,indent=2),encoding='utf-8')
                    if len(actions)!=1:raise RuntimeError(f'native overwrite Yes action ambiguous: {controls}')
                    yes=actions[0]
                    user.SendMessageW(yes,0x00F5,0,0);time.sleep(.7)  # BM_CLICK
                owned()
            stage("existing_open_idle", 4)
            cad.click(hwnd, 600, 160)
            # Existing-file Ctrl+S path, then repeats after the allocator has settled.
            key(0x11, 0x23)  # Ctrl+End
            user.PostMessageW(hwnd, 0x0102, ord('x'), 1)
            stage("existing_document_edited")
            ctrl_s(); stage("existing_ctrl_s_1")
            if doc.read_text(encoding='utf-8') != content+'x':
                raise RuntimeError('existing file edit/save did not reach disk')
            ctrl_s(); stage("existing_ctrl_s_repeat")
            # Blank-document Save As cancellation then confirmed first save, then repeat.
            # Ctrl+N and discard the current clean document, then cancel the blank Save As.
            key(0x11, 0x4E); time.sleep(.7); key(0x1B); time.sleep(.7)
            stage("new_document_idle")
            ctrl_s(); cancel_save_dialog(); stage("new_save_as_cancel")
            ctrl_s(); set_save_dialog(root / "first-save.md"); stage("new_first_save")
            ctrl_s(); stage("new_save_repeat")
            if not (root / "first-save.md").is_file():
                raise RuntimeError("first-save output file missing")
            if args.extended:
                cad.click(hwnd,600,160); key(0x10);key(0x11,0x41)
                body='中文保存 ÅÉg test'
                for ch in body:user.PostMessageW(hwnd,0x0102,ord(ch),1)
                time.sleep(.5)
                key(0x11,0x10,0x53);cancel_save_dialog();stage('dirty_save_as_cancel')
                assert not cad.direct_child_process_ids(proc.pid), 'canceled helper stayed alive'
                unicode_path=root/'保存 草稿.md'
                key(0x11,0x10,0x53);set_save_dialog(unicode_path);stage('unicode_first_save')
                assert unicode_path.read_text(encoding='utf-8')==body,'Unicode Save As bytes differ'
                key(0x11,0x10,0x53);set_save_dialog(doc,overwrite=True);stage('native_overwrite')
                # The app independently confirms replacing a different existing file.
                assert doc.read_text(encoding='utf-8')==content+'x','native prompt bypassed app conflict guard'
                # Click the app's explicit overwrite action, locating its row
                # from the focused Cancel outline in the actual owned pixels.
                from PIL import Image
                owned();w,h,raw=cad.capture_client(hwnd);im=Image.frombytes('RGBA',(w,h),raw,'raw','BGRA')
                unit=user.GetDpiForWindow(hwnd)/96
                logical_w=w/unit;width=min(520,max(220,logical_w-48));button=(width-64)/3
                cancel_x=(logical_w-width)/2+24+2*(button+8)
                ys=[]
                for y in range(h//4,h*3//4):
                    for x in range(round(cancel_x*unit),round((cancel_x+button)*unit)):
                        r,g,b,_=im.getpixel((x,y))
                        if b>130 and b>r*1.25 and b>g*1.08:ys.append(y)
                assert ys,'app conflict action row absent'
                cad.click(hwnd,round(((logical_w-width)/2+24+button/2)*unit),round((min(ys)+max(ys))/2))
                stage('app_overwrite_confirmed')
                assert doc.read_text(encoding='utf-8')==body,'native overwrite bytes differ'
                assert not cad.direct_child_process_ids(proc.pid), 'successful helper stayed alive'
                # Terminate only our known direct-child picker to simulate abnormal exit.
                # The parent editor must re-enable and keep its unsaved text.
                key(0x11,0x10,0x53);save_dialog()
                children=cad.direct_child_process_ids(proc.pid)
                assert len(children)==1,'expected one owned helper'
                fault_handle=kernel.OpenProcess(0x001001,False,children[0])
                assert fault_handle
                kernel.TerminateProcess.argtypes=[wintypes.HANDLE,wintypes.UINT]
                kernel.TerminateProcess.restype=wintypes.BOOL
                assert kernel.TerminateProcess(fault_handle,73)
                kernel.CloseHandle(fault_handle);time.sleep(.8);owned()
                user.IsWindowEnabled.argtypes=[wintypes.HWND]
                assert user.IsWindowEnabled(hwnd),'editor disabled after helper abnormal exit'
                cad.click(hwnd,600,160);key(0x11,0x23);key(0x10);user.PostMessageW(hwnd,0x0102,ord('x'),1)
                ctrl_s();stage('save_after_helper_failure')
                assert doc.read_text(encoding='utf-8')==body+'x','helper failure lost editor state'
                assert not cad.direct_child_process_ids(proc.pid), 'helper remains after fault test'
        finally:
            stop.set()
            if 'thread' in locals(): thread.join(timeout=2)
            kernel.CloseHandle(handle)
            if proc.poll() is None and hwnd:
                # Cancel only native dialogs owned by this test process.
                owned_pids = {proc.pid, *cad.direct_child_process_ids(proc.pid)}
                @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                def dismiss(candidate, _):
                    pid=wintypes.DWORD(); user.GetWindowThreadProcessId(candidate, ctypes.byref(pid))
                    cls=ctypes.create_unicode_buffer(64); user.GetClassNameW(candidate,cls,64)
                    if pid.value in owned_pids and cls.value=='#32770': user.PostMessageW(candidate,0x0010,0,0)
                    return True
                user.EnumWindows(dismiss,0)
                time.sleep(.4)
                user.PostMessageW(hwnd, 0x0010, 0, 0)
                try: proc.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    # Explicitly discard only this isolated fixture if an assertion
                    # left its unsaved-close prompt open. Never touch a foreign PID.
                    from PIL import Image
                    owned();w,h,raw=cad.capture_client(hwnd);im=Image.frombytes('RGBA',(w,h),raw,'raw','BGRA')
                    unit=user.GetDpiForWindow(hwnd)/96;logical_w=w/unit;width=min(520,max(220,logical_w-48));button=(width-64)/3
                    cancel_x=(logical_w-width)/2+24+2*(button+8);ys=[]
                    for y in range(h//4,h*3//4):
                        for x in range(round(cancel_x*unit),round((cancel_x+button)*unit)):
                            r,g,b,_=im.getpixel((x,y))
                            if b>130 and b>r*1.25 and b>g*1.08:ys.append(y)
                    if not ys:raise RuntimeError(f'owned PID {proc.pid} has no safe close prompt')
                    cad.click(hwnd,round(((logical_w-width)/2+24+(button+8)+button/2)*unit),round((min(ys)+max(ys))/2))
                    proc.wait(timeout=8)
            metadata = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                            document_bytes=initial_document_bytes, normalized_body_bytes=len(content.encode('utf-8')), final_document_bytes=doc.stat().st_size, requested_size_kib=args.size_kib,
                            pid=proc.pid, exit_code=proc.returncode,
                            units="MiB (bytes / 1048576)", sampling_interval_seconds=.05,
                            stages=stages, samples=samples)
            (out / "memory-save-trace.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps({k: metadata[k] for k in ("exe", "sha256", "document_bytes", "exit_code", "stages")}, ensure_ascii=False))


if __name__ == "__main__":
    main()
