from pathlib import Path


def read_note(path: Path) -> str:
    return path.read_text(encoding="utf-8")


if __name__ == "__main__":
    print(read_note(Path("notes.md")))
