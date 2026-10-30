"""Documentation failures must stop the checker without requiring a build."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_docs", ROOT / "scripts/check-docs.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class DocumentationChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.catalog = {"schema": 1, "documents": [
            {"path": "docs/contract.md", "title": "契约", "section": "维护", "summary": "当前契约"}
        ], "sync_rules": [{"topic": "输入", "sources": ["components/*"], "docs": ["docs/contract.md"]}]}
        self.write("docs/contract.md", "# 契约\n")
        for name in checker.ENTRYPOINTS:
            self.write(name, "# Product\nCurrent source 1.2.3\n")
        self.write("cmake/NeoEditorVersion.cmake", 'set(NEO_EDITOR_VERSION "1.2.3")\n')
        self.save_catalog()

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def save_catalog(self):
        self.write(checker.CATALOG, json.dumps(self.catalog, ensure_ascii=False))
        self.write(checker.INDEX, checker.render_index(self.catalog))

    def rules(self):
        return {issue["rule"] for issue in checker.check(self.root)["issues"]}

    def test_complete_fresh_clone_without_local_records_passes(self):
        self.assertTrue(checker.check(self.root)["ok"])

    def test_unregistered_and_missing_document_fail(self):
        self.write("docs/unregistered.md", "# New")
        (self.root / "docs/contract.md").unlink()
        self.assertIn("DOC001", self.rules())

    def test_examples_are_fixtures_not_maintained_docs(self):
        self.write("docs/examples/sample.md", "[example](missing.md)")
        self.assertTrue(checker.check(self.root)["ok"])

    def test_duplicate_document_fails(self):
        self.catalog["documents"] *= 2
        self.save_catalog()
        self.assertIn("DOC001", self.rules())

    def test_removed_sync_target_fails(self):
        self.catalog["sync_rules"][0]["docs"] = ["gone.md"]
        self.save_catalog()
        self.assertIn("DOC001", self.rules())

    def test_malformed_catalog_is_reported(self):
        for value in ("{broken", json.dumps({"schema": 1, "documents": 7, "sync_rules": []})):
            with self.subTest(value=value):
                self.write(checker.CATALOG, value)
                self.assertIn("DOC001", self.rules())

    def test_stale_navigation_fails(self):
        self.write(checker.INDEX, "# old navigation\n")
        self.assertIn("DOC002", self.rules())

    def test_inline_image_and_reference_dead_links_fail(self):
        for text in ("[x](gone.md)", "![x](gone.png)", "[x][ref]\n\n[ref]: gone.md"):
            with self.subTest(text=text):
                self.write("docs/contract.md", text)
                self.assertIn("DOC003", self.rules())

    def test_encoded_names_parentheses_and_angle_paths_resolve(self):
        self.write("docs/图 (1).png", "fixture")
        self.write("docs/contract.md", "![x](%E5%9B%BE%20(1).png)\n![x](<图 (1).png>)")
        self.assertTrue(checker.check(self.root)["ok"])

    def test_example_code_and_external_urls_are_not_local_links(self):
        self.write("docs/contract.md", '```md\n[x](missing.md)\n```\n`[x](other.md)`\n[x](https://example.org)\n[x](#anchor)')
        self.assertTrue(checker.check(self.root)["ok"])

    def test_local_evidence_is_reported_even_when_absent(self):
        self.write("docs/contract.md", "[run](../out/run/log.txt)\n[history](../local-docs/old.md)")
        result = checker.check(self.root)
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["local_references"]), 2)
        self.assertTrue(all(not item["present"] for item in result["local_references"]))

    def test_public_entrypoint_cannot_depend_on_local_evidence(self):
        self.write("README.md", "Source 1.2.3\n[run](out/run/log.txt)")
        self.assertIn("DOC004", self.rules())

    def test_relative_link_outside_repo_fails(self):
        self.write("docs/contract.md", "[outside](../../outside.md)")
        self.assertIn("DOC003", self.rules())

    def test_stale_source_version_fails(self):
        self.write("README.md", "Source 1.2.2")
        self.assertIn("DOC005", self.rules())

    def test_changed_nested_source_reports_document_review(self):
        result = checker.impacts(self.catalog, ["components/nested/input.h", "docs/contract.md"])
        self.assertEqual(result[0]["sources"], ["components/nested/input.h"])
        self.assertEqual(result[0]["changed_docs"], ["docs/contract.md"])
        self.assertEqual(checker.impacts(self.catalog, ["unrelated.txt"]), [])

    def test_git_paths_preserve_unicode_spaces_and_untracked_files(self):
        with patch.object(checker.subprocess, "check_output", side_effect=[
                "components/中文 名称.h\0".encode(), b"docs/new.md\0"]):
            self.assertEqual(checker.changed_files(self.root, "HEAD"), ["components/中文 名称.h", "docs/new.md"])

    def test_cli_failure_has_nonzero_exit_and_rule(self):
        self.write("docs/contract.md", "[broken](gone.md)")
        result = subprocess.run([sys.executable, str(ROOT / "scripts/check-docs.py"),
                                 "--root", str(self.root), "--json"], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(result.returncode, 1)
        self.assertIn("DOC003", result.stdout)

    def complete_review(self, changed):
        checker.sync_check(self.root, self.catalog, changed, draft=True)
        review = checker.read_review(self.root)
        for name in review["documents"]:
            review["documents"][name] = "核对：仅调整内部实现，接口与操作保持一致。"
        self.write(checker.REVIEW, json.dumps(review, ensure_ascii=False))

    def test_source_only_change_and_unrelated_doc_edit_cannot_pass(self):
        self.write("components/input.h", "// changed input")
        self.write("README.md", "Unrelated edit, source 1.2.3")
        result = checker.sync_check(self.root, self.catalog, ["components/input.h", "README.md"])
        self.assertTrue(any(i["rule"] == "DOC006" for i in result["issues"]))

    def test_hashes_with_blank_document_decisions_cannot_pass(self):
        self.write("components/input.h", "// changed")
        result = checker.sync_check(self.root, self.catalog, ["components/input.h"], draft=True)
        self.assertTrue(any(i["file"] == "docs/contract.md" for i in result["issues"]))

    def test_internal_change_with_explicit_review_can_pass(self):
        self.write("components/input.h", "// refactor")
        self.complete_review(["components/input.h"])
        self.assertEqual(checker.sync_check(self.root, self.catalog, ["components/input.h"])["issues"], [])

    def test_edit_after_review_fails_and_redraft_clears_decision(self):
        self.write("components/input.h", "// first")
        self.complete_review(["components/input.h"])
        self.write("components/input.h", "// second")
        result = checker.sync_check(self.root, self.catalog, ["components/input.h"])
        self.assertTrue(any(i["file"] == "components/input.h" for i in result["issues"]))
        checker.sync_check(self.root, self.catalog, ["components/input.h"], draft=True)
        self.assertEqual(checker.read_review(self.root)["documents"]["docs/contract.md"], "")

    def test_every_related_document_needs_a_decision(self):
        self.write("components/input.h", "// change")
        self.complete_review(["components/input.h"])
        self.write("docs/another.md", "# another")
        self.catalog["sync_rules"][0]["docs"].append("docs/another.md")
        result = checker.sync_check(self.root, self.catalog, ["components/input.h"])
        self.assertTrue(any(i["file"] == "docs/another.md" for i in result["issues"]))

    def test_lf_crlf_checkout_does_not_invalidate_review(self):
        self.write("components/input.h", "// text\n// next\n")
        self.complete_review(["components/input.h"])
        (self.root / "components/input.h").write_bytes(b"// text\r\n// next\r\n")
        self.assertEqual(checker.sync_check(self.root, self.catalog, ["components/input.h"])["issues"], [])

    def test_deleted_source_needs_its_own_review(self):
        self.write("components/input.h", "// text")
        self.complete_review(["components/input.h"])
        (self.root / "components/input.h").unlink()
        result = checker.sync_check(self.root, self.catalog, ["components/input.h"])
        self.assertTrue(any(i["file"] == "components/input.h" for i in result["issues"]))

    def test_unmapped_new_module_fails(self):
        self.write("new_engine/layout.cpp", "// new module")
        result = checker.sync_check(self.root, self.catalog, ["new_engine/layout.cpp"])
        self.assertTrue(any(i["rule"] == "DOC007" for i in result["issues"]))

    def test_review_of_second_file_keeps_previous_committed_sources(self):
        self.write("components/first.h", "// first")
        self.complete_review(["components/first.h"])
        self.write("components/second.h", "// second")
        self.complete_review(["components/second.h"])
        result = checker.sync_check(self.root, self.catalog, ["components/first.h", "components/second.h"])
        self.assertEqual(result["issues"], [])

    def test_staged_rename_and_untracked_unicode_keep_both_modules(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write("components/old.h", "// old module")
        subprocess.run(["git", "-C", str(self.root), "add", "components/old.h"], check=True)
        base = checker.git(self.root, "write-tree").strip()
        (self.root / "components/old.h").rename(self.root / "components/new.h")
        subprocess.run(["git", "-C", str(self.root), "add", "-A", "components"], check=True)
        self.write("components/中文 新文件.h", "// new")
        changed = checker.changed_files(self.root, base)
        for name in ("components/old.h", "components/new.h", "components/中文 新文件.h"):
            self.assertIn(name, changed)

    def test_push_base_covers_multiple_commits(self):
        self.assertEqual(checker.ci_base(self.root, {"before": "abc123"}, "push"), "abc123")

    def test_pr_and_new_branch_use_merge_base(self):
        with patch.object(checker, "git", return_value="common\n") as run:
            self.assertEqual(checker.ci_base(self.root, {"pull_request": {"base": {"sha": "base"}}}, "pull_request"), "common")
            self.assertIn("base", run.call_args.args)
            event = {"before": "0" * 40, "repository": {"default_branch": "main"}, "ref": "refs/heads/feature"}
            self.assertEqual(checker.ci_base(self.root, event, "push"), "common")
            self.assertIn("refs/remotes/origin/main", run.call_args.args)

    def test_initial_push_and_root_dispatch_compare_empty_tree(self):
        event = {"before": "0" * 40, "repository": {"default_branch": "main"}, "ref": "refs/heads/main"}
        with patch.object(checker, "git", return_value="empty\n"):
            self.assertEqual(checker.ci_base(self.root, event, "push"), "empty")
        with patch.object(checker, "git", side_effect=["head", "empty"]):
            self.assertEqual(checker.ci_base(self.root, {}, "workflow_dispatch"), "empty")

    def test_initial_push_empty_tree_is_usable_in_a_real_repository(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.write("components/input.h", "// initial source")
        subprocess.run(["git", "-C", str(self.root), "add", "components"], check=True)
        event = {"before": "0" * 40, "repository": {"default_branch": "main"}, "ref": "refs/heads/main"}
        base = checker.ci_base(self.root, event, "push")
        self.assertIn("components/input.h", checker.changed_files(self.root, base))

    def test_clean_committed_checkout_still_checks_push_range(self):
        self.write("components/input.h", "// committed without docs")
        # CI supplies the prior push SHA even though git diff HEAD is empty.
        with patch.object(checker, "git", side_effect=["components/input.h\0", ""]):
            changed = checker.changed_files(self.root, "prior-push-sha")
        self.assertTrue(checker.sync_check(self.root, self.catalog, changed)["issues"])

    def test_cli_ci_range_rejects_unreviewed_source_and_accepts_completed_review(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        before = checker.git(self.root, "write-tree").strip()
        self.write("components/input.h", "// pending source")
        subprocess.run(["git", "-C", str(self.root), "add", "components"], check=True)
        final_tree = checker.git(self.root, "write-tree").strip()
        event = self.root / "event.json"
        event.write_text(json.dumps({"before": before}), encoding="utf-8")
        env = {**os.environ, "GITHUB_EVENT_NAME": "push", "GITHUB_EVENT_PATH": str(event), "PYTHONIOENCODING": "utf-8"}
        command = [sys.executable, str(ROOT / "scripts/check-docs.py"), "--root", str(self.root), "--json"]
        # Comparing only the final tree misses the source; the push range finds it.
        no_changes = subprocess.run(command + ["--base", final_tree], capture_output=True, env=env)
        self.assertEqual(no_changes.returncode, 0, no_changes.stdout)
        missing = subprocess.run(command + ["--ci"], capture_output=True, env=env)
        self.assertEqual(missing.returncode, 1)
        self.assertIn(b"DOC006", missing.stdout)
        self.complete_review(["components/input.h"])
        reviewed = subprocess.run(command + ["--ci"], capture_output=True, env=env)
        self.assertEqual(reviewed.returncode, 0, reviewed.stdout)


    def test_cli_utf8_output_under_english_windows_encoding(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        base = checker.git(self.root, "write-tree").strip()
        self.write("components/input.h", "// pending source")
        env = {**os.environ, "PYTHONIOENCODING": "cp1252", "PYTHONUTF8": "0"}
        command = [sys.executable, str(ROOT / "scripts/check-docs.py"),
                   "--root", str(self.root), "--base", base]
        for reviewed in (False, True):
            if reviewed:
                self.complete_review(["components/input.h"])
            for json_output in (False, True):
                with self.subTest(reviewed=reviewed, json_output=json_output):
                    result = subprocess.run(command + (["--json"] if json_output else []),
                                            capture_output=True, env=env)
                    output = result.stdout.decode("utf-8")
                    self.assertEqual(result.returncode, 0 if reviewed else 1,
                                     result.stderr.decode("utf-8"))
                    self.assertNotIn(b"UnicodeEncodeError", result.stderr)
                    if json_output:
                        self.assertEqual(json.loads(output)["ok"], reviewed)
                    else:
                        self.assertIn("文档检查：" + ("PASS" if reviewed else "FAIL"), output)
                    if not reviewed:
                        self.assertIn("DOC006", output)
                        self.assertIn("核对", output)


class RepositoryDocumentationMapping(unittest.TestCase):
    def test_ci_checks_source_only_pushes_with_full_history(self):
        workflow = (ROOT / ".github/workflows/docs.yml").read_text(encoding="utf-8-sig")
        self.assertNotIn("paths:", workflow)
        self.assertIn("fetch-depth: 0", workflow)
        self.assertIn("scripts/check-docs.py --ci", workflow)

    def test_known_model_and_platform_changes_reach_their_contracts(self):
        catalog = json.loads((ROOT / checker.CATALOG).read_text(encoding="utf-8-sig"))
        contracts = {
            "apps/neo_editor/model/atomic_write.cpp": "docs/NeoEditor-多文档会话与关闭恢复技术说明.md",
            "apps/neo_editor/model/lp_decorations.h": "docs/块模型契约-2026-09-26.md",
            "apps/neo_editor/platform/vault_watcher.cpp": "docs/NeoEditor-通用文本文件与设置确认说明-2026-10-02.md",
            "apps/neo_editor/model/i18n_ui.inc": "README_EN.md",
            "apps/neo_editor/platform/window_chrome.h": "docs/当前实现与验收路线.md",
            "apps/neo_editor/model/font_catalog.cpp": "docs/NeoEditor-通用文本文件与设置确认说明-2026-10-02.md",
            "apps/neo_editor/model/heading_anchor.h": "docs/NeoEditor-通用文本文件与设置确认说明-2026-10-02.md",
            "apps/neo_editor/model/settings.cpp": "docs/NeoEditor主题导入.md",
            "apps/neo_editor/CMakeLists.txt": "docs/NeoEditor-发布构建说明-2026-10-02.md",
            "tests/unit/find_actions.cpp": "docs/查找替换与选区范围.md",
        }
        for source, document in contracts.items():
            with self.subTest(source=source):
                targets = {d for impact in checker.impacts(catalog, [source]) for d in impact["review_docs"]}
                self.assertIn(document, targets)

    def test_all_managed_source_files_have_mapping(self):
        catalog = json.loads((ROOT / checker.CATALOG).read_text(encoding="utf-8-sig"))
        files = checker.git(ROOT, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split("\0")
        managed = [name for name in files if name and checker.is_source(name) and
                   (name.startswith(checker.SOURCE_ROOTS) or name in ("CMakeLists.txt", "eui_neo.h"))]
        mapped = {name for item in checker.impacts(catalog, managed) for name in item["sources"]}
        self.assertEqual(set(managed) - mapped, set())


if __name__ == "__main__":
    unittest.main()
