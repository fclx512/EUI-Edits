"""Check maintained docs; --write-index regenerates navigation only.

Uses only Python's standard library. Rule IDs are documented in the development
guide. Local evidence references are inventoried but need not exist in a clone.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
CATALOG = "docs/文档清单.json"
INDEX = "docs/目录说明.md"
REVIEW = "docs/文档同步记录.json"
# New source directories still require a mapping instead of disappearing from
# the impact report. Markdown remains documentation, including test samples.
SOURCE_ROOTS = ("apps/neo_editor/", "core/", "components/", "include/", "cmake/",
                "scripts/", "tests/", "assets/", "3rd/", ".github/workflows/")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".inc", ".inl",
                   ".cmake", ".ps1", ".py", ".js", ".mjs", ".cjs", ".yml", ".yaml",
                   ".rc", ".manifest", ".bat", ".cmd", ".sh"}
ENTRYPOINTS = ("AGENTS.md", "README.md", "README_EN.md",
               "apps/neo_editor/README.md", "apps/neo_editor/RELEASE-NOTES.md",
               "apps/neo_editor/THIRD-PARTY-NOTICES.md")
LOCAL_ROOTS = ("local-docs", "out", "参考")
LINK = re.compile(r'!?\[[^\]\n]*\]\(\s*(<[^>\n]+>|(?:\\.|[^()\s]|\([^()\n]*\))+)(?:\s+"[^"]*")?\s*\)')
REFERENCE = re.compile(r'^ {0,3}\[[^\]\n]+\]:\s*(<[^>\n]+>|\S+)', re.M)


def links(text: str) -> list[str]:
    # Examples of Markdown links inside code are not actual links.
    text = re.sub(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n.*?^ {0,3}\1[^\n]*$', '', text, flags=re.M | re.S)
    text = re.sub(r'(`+)[^\n]*?\1', '', text)
    return [m.group(1).strip('<>') for pattern in (LINK, REFERENCE)
            for m in pattern.finditer(text)]


def render_index(catalog: dict) -> str:
    lines = ["# 文档导航", "",
             "> 由 `docs/文档清单.json` 生成；修改清单后运行 `python scripts/check-docs.py --write-index`。", "",
             "接手先读开发与维护规范，再按任务阅读当前路线和技术契约。", ""]
    sections = dict.fromkeys(item["section"] for item in catalog["documents"])
    for section in sections:
        lines.extend([f"## {section}", "", "| 文档 | 内容 |", "| --- | --- |"])
        for item in catalog["documents"]:
            if item["section"] == section:
                relative = item["path"].removeprefix("docs/")
                lines.append(f'| [{item["title"]}]({relative}) | {item["summary"]} |')
        lines.append("")
    lines.extend(["## 改动时同步哪些文档", "",
                  "源码改动须完成对应文档核对，并写入 `docs/文档同步记录.json`：更新了什么，或为何无需更新。记录绑定源码内容，代码再次变化后重新核对。具体命令见开发与维护规范。", "",
                  "| 改动 | 核对与同步 |", "| --- | --- |"])
    for rule in catalog["sync_rules"]:
        targets = []
        for name in rule["docs"]:
            target = name.removeprefix("docs/") if name.startswith("docs/") else "../" + name
            title = next((d["title"] for d in catalog["documents"] if d["path"] == name), name)
            targets.append(f"[{title}]({target})")
        lines.append(f'| {rule["topic"]} | {"、".join(targets)} |')
    lines.extend(["", "## 本地记录与证据", "",
                  "- `local-docs/repo-docs/`：轮次记录、交接与旧计划；`local-docs/performance/`：测量报告。",
                  "- `out/`：日志、截图、数据、配置和资产哈希；预览与候选目录按开发规范管理。",
                  "- 这些目录只在当前克隆保留。检查器列出指向本地材料的引用，缺失不影响新检出的文档检查。",
                  "- 带日期的技术契约文件沿用原文件名，内容随实现维护；日期报告归入本地记录。", ""])
    return "\n".join(lines)


def check(root: Path) -> dict:
    issues: list[dict] = []
    local: list[dict] = []

    def fail(rule: str, file: str, message: str) -> None:
        issues.append({"rule": rule, "file": file, "message": message})

    try:
        catalog = json.loads((root / CATALOG).read_text(encoding="utf-8-sig"))
        if catalog["schema"] != 1:
            raise ValueError("unsupported schema")
        documents = catalog["documents"]
        rules = catalog["sync_rules"]
        if not isinstance(documents, list) or not documents or not isinstance(rules, list):
            raise ValueError("documents and sync_rules must be lists")
        for item in documents:
            for key in ("path", "title", "section", "summary"):
                if not isinstance(item[key], str) or not item[key].strip():
                    raise ValueError(f"invalid document {key}")
            if not item["path"].startswith("docs/") or ".." in Path(item["path"]).parts or "\\" in item["path"]:
                raise ValueError("document path must be relative to docs/")
        for rule in rules:
            if (not isinstance(rule["topic"], str) or not rule["topic"] or
                    any(not isinstance(rule[key], list) or not rule[key] or
                        any(not isinstance(value, str) or not value for value in rule[key])
                        for key in ("sources", "docs"))):
                raise ValueError("empty sync rule")
    except (OSError, ValueError, KeyError, TypeError) as error:
        fail("DOC001", CATALOG, str(error))
        return {"ok": False, "issues": issues, "local_references": local}

    names = [item["path"] for item in documents]
    actual = {p.relative_to(root).as_posix() for p in (root / "docs").rglob("*.md")
              if p.relative_to(root / "docs").parts[0] != "examples"}
    expected = set(names) | {INDEX}
    for name in sorted(actual - expected):
        fail("DOC001", name, "未登记到文档清单")
    for name in sorted(expected - actual):
        fail("DOC001", name, "清单中的文档不存在")
    if len(names) != len(set(names)):
        fail("DOC001", CATALOG, "文档重复登记")
    for rule in rules:
        for name in rule["docs"]:
            if not (root / name).is_file():
                fail("DOC001", CATALOG, f"同步目标不存在：{name}")

    index = root / INDEX
    if index.is_file() and index.read_text(encoding="utf-8-sig") != render_index(catalog):
        fail("DOC002", INDEX, "导航过期；运行 python scripts/check-docs.py --write-index")

    for name in sorted(actual | set(ENTRYPOINTS) | {name for rule in rules for name in rule["docs"]}):
        path = root / name
        if not path.is_file():
            fail("DOC003", name, "维护入口不存在")
            continue
        text = path.read_text(encoding="utf-8-sig")
        for raw in links(text):
            parts = urlsplit(raw)
            if parts.scheme or parts.netloc or not parts.path:
                continue
            target = (path.parent / unquote(parts.path)).resolve()
            try:
                relative = target.relative_to(root.resolve()).as_posix()
            except ValueError:
                fail("DOC003", name, f"相对链接超出仓库：{raw}")
                continue
            if relative.split("/")[0] in LOCAL_ROOTS:
                local.append({"file": name, "target": relative, "present": target.exists()})
            elif not target.exists():
                fail("DOC003", name, f"链接目标不存在：{raw}")
        # Checked-in public documentation must work without local evidence.
        if name in ENTRYPOINTS and any(ref["file"] == name for ref in local):
            fail("DOC004", name, "对外入口应链接稳定文档，本地证据放轮次记录")

    version_file = root / "cmake/NeoEditorVersion.cmake"
    if version_file.is_file():
        match = re.search(r'set\(NEO_EDITOR_VERSION "([^"\s]+)"', version_file.read_text(encoding="utf-8-sig"))
        if not match:
            fail("DOC005", version_file.relative_to(root).as_posix(), "无法读取产品版本")
        else:
            # Current introduction, before history, must identify the source version.
            version = match[1]
            for name in ("README.md", "README_EN.md", "apps/neo_editor/README.md", "apps/neo_editor/RELEASE-NOTES.md"):
                path = root / name
                if path.is_file() and not re.search(rf'(?<![\d.]){re.escape(version)}(?![\d.])', path.read_text(encoding="utf-8-sig")[:6000]):
                    fail("DOC005", name, f"开篇说明缺少当前源码版本 {version}")
    return {"ok": not issues, "issues": issues, "local_references": local}


def git(root: Path, *args: str, input: bytes | None = None) -> str:
    return subprocess.check_output(["git", "-C", str(root), "-c", "core.safecrlf=false", *args], input=input).decode("utf-8")


def changed_files(root: Path, base: str) -> list[str]:
    # Includes staged, unstaged and untracked additions; deleted paths still matter.
    # Disable rename folding so both the removed contract and its new location
    # contribute to documentation review, including cross-module moves.
    return sorted(set(git(root, "diff", "--no-renames", "--name-only", "-z", base, "--").split("\0") +
                      git(root, "ls-files", "--others", "--exclude-standard", "-z").split("\0")) - {""})


def ci_base(root: Path, event: dict, event_name: str) -> str:
    if event_name == "pull_request":
        return git(root, "merge-base", "HEAD", event["pull_request"]["base"]["sha"]).strip()
    if event_name == "push":
        before = event["before"]
        if before and set(before) != {"0"}:
            return before
        branch = event["repository"]["default_branch"]
        if event["ref"] != "refs/heads/" + branch:
            return git(root, "merge-base", "HEAD", "refs/remotes/origin/" + branch).strip()
        return git(root, "hash-object", "-t", "tree", "--stdin", input=b"").strip()
    if event_name == "workflow_dispatch":
        parents = git(root, "rev-list", "--parents", "-n", "1", "HEAD").split()
        return parents[1] if len(parents) > 1 else git(root, "hash-object", "-t", "tree", "--stdin", input=b"").strip()
    raise ValueError(f"unsupported CI event: {event_name}")


def is_source(name: str) -> bool:
    return not name.endswith(".md") and (name == "CMakeLists.txt" or
            name.startswith(SOURCE_ROOTS) or Path(name).suffix.lower() in SOURCE_SUFFIXES)


def source_hash(root: Path, name: str) -> str:
    path = root / name
    if not path.exists():
        return "deleted"
    data = path.read_bytes()
    # Git's LF/CRLF checkout conversion must not invalidate a reviewed source.
    if b"\0" not in data:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def read_review(root: Path) -> dict:
    path = root / REVIEW
    if not path.exists():
        return {"schema": 1, "sources": {}, "documents": {}}
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if data["schema"] != 1 or any(not isinstance(data[key], dict) for key in ("sources", "documents")):
        raise ValueError("invalid documentation review schema")
    if any(not isinstance(value, str) for key in ("sources", "documents") for value in data[key].values()):
        raise ValueError("review hashes and explanations must be strings")
    return data


def sync_check(root: Path, catalog: dict, changed: list[str], draft: bool = False) -> dict:
    affected = impacts(catalog, changed)
    sources = [name for name in changed if is_source(name)]
    mapped = {name for item in affected for name in item["sources"]}
    issues = [{"rule": "DOC007", "file": name, "message": "源码缺少文档映射；在文档清单的 sync_rules 中补充"}
              for name in sources if name not in mapped]
    required = sorted({name for item in affected for name in item["review_docs"]})
    try:
        review = read_review(root)
    except (OSError, ValueError, KeyError, TypeError) as error:
        issues.append({"rule": "DOC006", "file": REVIEW, "message": str(error)})
        return {"issues": issues, "impacts": affected}
    if draft and sources:
        for name in sources:
            signature = source_hash(root, name)
            if review["sources"].get(name) != signature:
                for item in affected:
                    if name in item["sources"]:
                        for document in item["review_docs"]:
                            review["documents"][document] = ""
            review["sources"][name] = signature
        for name in required:
            review["documents"].setdefault(name, "")
        (root / REVIEW).write_text(json.dumps(review, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")
    for name in sources:
        if review["sources"].get(name) != source_hash(root, name):
            issues.append({"rule": "DOC006", "file": name, "message": "本次内容尚未核对文档；运行 --write-review 生成待填写记录"})
    for name in required:
        if not review["documents"].get(name, "").strip():
            issues.append({"rule": "DOC006", "file": name, "message": "在文档同步记录中填写更新内容，或无需更新的具体原因"})
    return {"issues": issues, "impacts": affected,
            "document_reviews": {name: review["documents"].get(name, "") for name in required}}


def impacts(catalog: dict, changed: list[str]) -> list[dict]:
    result = []
    for rule in catalog["sync_rules"]:
        sources = [name for name in changed if is_source(name) and
                   any(fnmatch.fnmatchcase(name, pattern) for pattern in rule["sources"])]
        if sources:
            result.append({"topic": rule["topic"], "sources": sources,
                           "review_docs": rule["docs"],
                           "changed_docs": [name for name in rule["docs"] if name in changed]})
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--write-index", action="store_true")
    comparison = parser.add_mutually_exclusive_group()
    comparison.add_argument("--base", help="compare against this Git ref (default: HEAD)")
    comparison.add_argument("--ci", action="store_true", help="compare the complete GitHub push/PR range")
    parser.add_argument("--write-review", action="store_true", help="update the review draft; fill explanations before checking again")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.write_index:
            catalog = json.loads((root / CATALOG).read_text(encoding="utf-8-sig"))
            (root / INDEX).write_text(render_index(catalog), encoding="utf-8", newline="\n")
        result = check(root)
        if result["ok"]:
            catalog = json.loads((root / CATALOG).read_text(encoding="utf-8-sig"))
            base = args.base or "HEAD"
            if args.ci:
                event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
                base = ci_base(root, event, os.environ["GITHUB_EVENT_NAME"])
            sync = sync_check(root, catalog, changed_files(root, base), args.write_review)
            result["base"] = base
            result["impacts"] = sync["impacts"]
            result["document_reviews"] = sync.get("document_reviews", {})
            result["issues"].extend(sync["issues"])
            result["ok"] = not result["issues"]
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        result = {"ok": False, "issues": [{"rule": "DOC000", "file": "check-docs.py", "message": str(error)}], "local_references": []}
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        for issue in result["issues"]:
            print(f'{issue["rule"]} {issue["file"]}: {issue["message"]}')
        print(f'文档检查：{"PASS" if result["ok"] else "FAIL"}；本地引用 {len(result["local_references"])} 项（详情用 --json）。')
        for impact in result.get("impacts", []):
            print(f'核对 {impact["topic"]}：' + "、".join(impact["review_docs"]))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    # Windows CI redirects output under an English code page. Keep Chinese
    # diagnostics and JSON intact regardless of the runner's locale.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    raise SystemExit(main())
