"""Small Win32 screen capture and input helpers for isolated EUI-Edits probes."""

import ctypes
import os
import struct
import time
import zlib
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

TH32CS_SNAPPROCESS = 0x00000002
SYNCHRONIZE = 0x00100000
INVALID_HANDLE = ctypes.c_void_p(-1).value
# 与 apps/neo_editor/platform/single_instance.cpp 保持一致：
# 互斥名带 Local\ 命名空间，APPDATA 隔离绕不开它。
MUTEX_NAME = "Local\\EUI-Edits.SingleInstance"
FORWARD_FILE = "EUI-Edits.next-open"

kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.CreateToolhelp32Snapshot.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
kernel32.Process32FirstW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.Process32NextW.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
kernel32.OpenMutexW.restype = ctypes.c_void_p
kernel32.OpenMutexW.argtypes = [ctypes.c_uint32, ctypes.c_bool, ctypes.c_wchar_p]
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_uint32), ("cntUsage", ctypes.c_uint32),
                ("th32ProcessID", ctypes.c_uint32), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", ctypes.c_uint32), ("cntThreads", ctypes.c_uint32),
                ("th32ParentProcessID", ctypes.c_uint32), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", ctypes.c_uint32), ("szExeFile", ctypes.c_wchar * 260)]


def process_ids(image_name):
    """PIDs whose image name matches (case-insensitive), via a Toolhelp snapshot."""
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snapshot or snapshot == INVALID_HANDLE:
        return []
    ids = []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                if entry.szExeFile.lower() == image_name.lower():
                    ids.append(entry.th32ProcessID)
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
    finally:
        kernel32.CloseHandle(snapshot)
    return ids


def direct_child_process_ids(parent_pid):
    """Read-only inventory of direct children for ownership of isolated pickers."""
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snapshot or snapshot == INVALID_HANDLE:
        return []
    ids = []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                if entry.th32ParentProcessID == parent_pid:
                    ids.append(entry.th32ProcessID)
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
    finally:
        kernel32.CloseHandle(snapshot)
    return ids


def single_instance_held():
    """True when another process owns the Local\\EUI-Edits.SingleInstance mutex."""
    handle = kernel32.OpenMutexW(SYNCHRONIZE, False, ctypes.c_wchar_p(MUTEX_NAME))
    if not handle:
        return False
    kernel32.CloseHandle(handle)
    return True


def forward_file_path():
    return os.path.join(os.environ.get("TEMP", ""), FORWARD_FILE)


def assert_no_foreign_instance(where=""):
    """Refuse to run when a user's EUI-Edits instance could swallow the launch.

    A second launch forwards its path through %TEMP% and exits, so the probe would
    drive (or be swallowed by) the user's real window. Never kill such a process.
    """
    import os as _os
    if _os.environ.get("NEO_PROBE_ALLOW_FOREIGN") == "1":
        # 探针以 NEO_SINGLE_INSTANCE=0 + 隔离 APPDATA 启动被测进程时，
        # 可与用户手上的实例并存；此时不再拒绝运行。
        return
    pids = process_ids("neo_editor.exe")
    if pids:
        raise RuntimeError(
            f"{where}: EUI-Edits 已在运行（PID: {', '.join(map(str, pids))}）；"
            "请先手动关闭后再跑探针（探针不会结束用户进程）")
    if single_instance_held():
        raise RuntimeError(
            f"{where}: 单实例互斥 {MUTEX_NAME} 已被占用（疑似残留实例），请确认后再跑探针")
    forward = forward_file_path()
    if os.path.isfile(forward):
        raise RuntimeError(
            f"{where}: 存在残留转发文件 {forward}，会劫持被测进程的启动文档；"
            "请确认无实例后删除该文件再重试")


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                ("biClrImportant", ctypes.c_uint32)]


def make_dpi_aware():
    try:
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except AttributeError:
        pass
    user32.SetProcessDPIAware()


def click(hwnd, x, y):
    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    user32.SetCursorPos(origin.x + x, origin.y + y)
    time.sleep(0.2)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.45)


def assert_unlocked(where=""):
    hwnd = user32.GetForegroundWindow()
    title = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title, len(title))
    if "锁屏" in title.value or "Lock" in title.value:
        raise RuntimeError(f"{where}: desktop is locked")


def ensure_foreground(hwnd, tries=10):
    kernel32 = ctypes.windll.kernel32
    for _ in range(tries):
        if user32.GetForegroundWindow() == hwnd:
            return True
        if user32.IsIconic(hwnd):
            user32.ShowWindow(hwnd, 9)
        foreground = user32.GetForegroundWindow()
        foreground_thread = (user32.GetWindowThreadProcessId(foreground, None)
                             if foreground else 0)
        own_thread = kernel32.GetCurrentThreadId()
        attached = bool(foreground_thread) and bool(
            user32.AttachThreadInput(own_thread, foreground_thread, True))
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
        if attached:
            user32.AttachThreadInput(own_thread, foreground_thread, False)
        time.sleep(0.35)
        if user32.GetForegroundWindow() == hwnd:
            return True
        user32.keybd_event(0x12, 0, 0, 0)
        user32.keybd_event(0x12, 0, 0x0002, 0)
        time.sleep(0.25)
    return user32.GetForegroundWindow() == hwnd


def capture_client(hwnd):
    """Copy the visible screen's client pixels; caller must ensure foreground."""
    origin = wintypes.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    rect = wintypes.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    width, height = rect.right, rect.bottom
    screen_dc = user32.GetDC(0)
    memory_dc = gdi32.CreateCompatibleDC(screen_dc)
    bitmap = gdi32.CreateCompatibleBitmap(screen_dc, width, height)
    gdi32.SelectObject(memory_dc, bitmap)
    gdi32.BitBlt(memory_dc, 0, 0, width, height,
                 screen_dc, origin.x, origin.y, 0x00CC0020)
    header = BITMAPINFOHEADER()
    header.biSize = ctypes.sizeof(header)
    header.biWidth = width
    header.biHeight = -height
    header.biPlanes = 1
    header.biBitCount = 32
    pixels = ctypes.create_string_buffer(width * height * 4)
    gdi32.GetDIBits(memory_dc, bitmap, 0, height, pixels, ctypes.byref(header), 0)
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(memory_dc)
    user32.ReleaseDC(0, screen_dc)
    return width, height, pixels.raw


def write_png(path, width, height, bgra):
    scanlines = bytearray()
    for y in range(height):
        row = bgra[y * width * 4:(y + 1) * width * 4]
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::4]
        rgb[1::3] = row[1::4]
        rgb[2::3] = row[0::4]
        scanlines.append(0)
        scanlines += rgb

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    blob = b"\x89PNG\r\n\x1a\n"
    blob += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    blob += chunk(b"IDAT", zlib.compress(bytes(scanlines), 6))
    blob += chunk(b"IEND", b"")
    with open(path, "wb") as target:
        target.write(blob)
