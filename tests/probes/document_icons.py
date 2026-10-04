"""Extract EUI-Edits document icon resources 102..106 without touching the registry.

Run from the repository root with:
    python tests/probes/document_icons.py [path-to-neo_editor.exe] [output-directory]
    python tests/probes/document_icons.py [path-to-neo_editor.exe] [output-directory] --all-sizes
Requires Windows and Pillow. Writes large (system icon size) and small (system small
icon size) PNGs so the embedded resources can be inspected visually. --all-sizes
also extracts each embedded 16/20/24/32/48/64/128/256px ICO frame.
"""

from __future__ import annotations

import argparse
import ctypes
import sys
from pathlib import Path
from ctypes import wintypes

from PIL import Image


if sys.platform != "win32":
    raise SystemExit("This resource probe requires Windows.")

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXE = ROOT / "build-win32/visual/neo_editor.exe"
ICON_SIZES = (16, 20, 24, 32, 48, 64, 128, 256)
LOAD_LIBRARY_AS_DATAFILE = 0x00000002
IMAGE_ICON = 1

shell32 = ctypes.WinDLL("shell32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

HICON = wintypes.HANDLE
HDC = wintypes.HANDLE
HBITMAP = wintypes.HANDLE
shell32.ExtractIconExW.argtypes = [
    wintypes.LPCWSTR, ctypes.c_int, ctypes.POINTER(HICON), ctypes.POINTER(HICON), wintypes.UINT
]
shell32.ExtractIconExW.restype = wintypes.UINT
kernel32.LoadLibraryExW.argtypes = [wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD]
kernel32.LoadLibraryExW.restype = wintypes.HMODULE
kernel32.FreeLibrary.argtypes = [wintypes.HMODULE]
kernel32.FreeLibrary.restype = wintypes.BOOL
user32.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT,
                              ctypes.c_int, ctypes.c_int, wintypes.UINT]
user32.LoadImageW.restype = HICON
user32.GetSystemMetrics.argtypes = [ctypes.c_int]
user32.GetSystemMetrics.restype = ctypes.c_int
user32.DrawIconEx.argtypes = [HDC, ctypes.c_int, ctypes.c_int, HICON, ctypes.c_int,
                              ctypes.c_int, wintypes.UINT, HDC, wintypes.UINT]
user32.DrawIconEx.restype = wintypes.BOOL
user32.DestroyIcon.argtypes = [HICON]
user32.DestroyIcon.restype = wintypes.BOOL
gdi32.CreateDIBSection.argtypes = [HDC, ctypes.c_void_p, wintypes.UINT,
                                   ctypes.POINTER(ctypes.c_void_p), HDC, wintypes.DWORD]
gdi32.CreateDIBSection.restype = HBITMAP
gdi32.CreateCompatibleDC.argtypes = [HDC]
gdi32.CreateCompatibleDC.restype = HDC
gdi32.DeleteDC.argtypes = [HDC]
gdi32.DeleteDC.restype = wintypes.BOOL
gdi32.SelectObject.argtypes = [HDC, wintypes.HANDLE]
gdi32.SelectObject.restype = wintypes.HANDLE
gdi32.DeleteObject.argtypes = [wintypes.HANDLE]
gdi32.DeleteObject.restype = wintypes.BOOL


class BitmapInfoHeader(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long),
        ("biHeight", ctypes.c_long), ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
        ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BitmapInfo(ctypes.Structure):
    _fields_ = [("bmiHeader", BitmapInfoHeader), ("bmiColors", wintypes.DWORD * 3)]


def last_error(message: str) -> OSError:
    return ctypes.WinError(ctypes.get_last_error(), message)


def save_icon(handle: HICON, path: Path, width: int, height: int) -> None:
    dc = gdi32.CreateCompatibleDC(None)
    if not dc:
        raise last_error("CreateCompatibleDC failed")
    info = BitmapInfo()
    info.bmiHeader = BitmapInfoHeader(
        ctypes.sizeof(BitmapInfoHeader), width, -height, 1, 32, 0, width * height * 4,
        0, 0, 0, 0,
    )
    pixels = ctypes.c_void_p()
    bitmap = gdi32.CreateDIBSection(dc, ctypes.byref(info), 0, ctypes.byref(pixels), None, 0)
    if not bitmap or not pixels:
        gdi32.DeleteDC(dc)
        raise last_error("CreateDIBSection failed")
    old_bitmap = gdi32.SelectObject(dc, bitmap)
    try:
        if not user32.DrawIconEx(dc, 0, 0, handle, width, height, 0, None, 3):
            raise last_error("DrawIconEx failed")
        raw = ctypes.string_at(pixels, width * height * 4)
        Image.frombytes("RGBA", (width, height), raw, "raw", "BGRA").save(path)
    finally:
        gdi32.SelectObject(dc, old_bitmap)
        gdi32.DeleteObject(bitmap)
        gdi32.DeleteDC(dc)


def extract_all_sizes(exe: Path, out: Path) -> None:
    """Load each native ICO frame by resource ID without registering file types."""
    module = kernel32.LoadLibraryExW(str(exe), None, LOAD_LIBRARY_AS_DATAFILE)
    if not module:
        raise last_error(f"LoadLibraryExW failed for {exe}")
    try:
        for resource_id, label in ((102, "txt"), (103, "md"), (104, "code"),
                                   (105, "data"), (106, "file")):
            resource_name = ctypes.cast(ctypes.c_void_p(resource_id), wintypes.LPCWSTR)
            for size in ICON_SIZES:
                icon = user32.LoadImageW(module, resource_name, IMAGE_ICON, size, size, 0)
                if not icon:
                    raise last_error(
                        f"LoadImageW failed for resource {resource_id} at {size}x{size}")
                try:
                    path = out / f"{label}_{size}.png"
                    save_icon(icon, path, size, size)
                    print(f"resource {resource_id}: {path} ({size}x{size})")
                finally:
                    user32.DestroyIcon(icon)
    finally:
        kernel32.FreeLibrary(module)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", nargs="?", type=Path, default=DEFAULT_EXE)
    parser.add_argument("out", nargs="?", type=Path, default=None)
    parser.add_argument("--all-sizes", action="store_true",
                        help="also extract every embedded ICO size (16 through 256px)")
    args = parser.parse_args()
    exe = args.exe.resolve()
    out = args.out.resolve() if args.out is not None else exe.parent / "document_icons"
    if not exe.is_file():
        raise SystemExit(f"Executable not found: {exe}")
    out.mkdir(parents=True, exist_ok=True)
    large_size = user32.GetSystemMetrics(11)  # SM_CXICON / SM_CYICON
    small_size = user32.GetSystemMetrics(49)  # SM_CXSMICON / SM_CYSMICON
    for resource_id, label in ((102, "txt"), (103, "md"), (104, "code"),
                               (105, "data"), (106, "file")):
        large, small = HICON(), HICON()
        count = shell32.ExtractIconExW(str(exe), -resource_id,
                                       ctypes.byref(large), ctypes.byref(small), 1)
        # ExtractIconExW counts a result in each requested output array (large + small).
        if count < 2 or not large.value or not small.value:
            raise last_error(f"ExtractIconExW could not extract icon resource {resource_id}")
        try:
            for size, name, handle in (
                (large_size, "large", large), (small_size, "small", small)
            ):
                path = out / f"{label}_{name}.png"
                save_icon(handle, path, size, size)
                with Image.open(path) as image:
                    print(f"resource {resource_id}: {path} ({image.width}x{image.height})")
        finally:
            user32.DestroyIcon(large)
            user32.DestroyIcon(small)
    if args.all_sizes:
        extract_all_sizes(exe, out)


if __name__ == "__main__":
    main()
