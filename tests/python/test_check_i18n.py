"""Offline checker regressions; no compiler, application or GUI is launched."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("check_i18n", ROOT / "tests/tools/check_i18n.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class BoundedMessageIds(unittest.TestCase):
    def scan(self, source, messages=None):
        messages = messages or {name: (name, name, "fixture") for name in ("a", "b", "c")}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = root / "apps/neo_editor"
            app.mkdir(parents=True)
            (app / "fixture.cpp").write_text(source, encoding="utf-8")
            with patch.object(checker, "ROOT", root), patch.object(checker, "APP", app):
                return checker.scan_calls(messages)

    def test_checks_both_ternary_branches(self):
        errors, dynamic, count = self.scan('i18n::tr(flag ? "a" : "b");')
        self.assertEqual((errors, dynamic, count), ([], [], 1))

    def test_missing_false_branch_is_an_error(self):
        errors, dynamic, count = self.scan('i18n::tr(flag ? "a" : "missing");')
        self.assertTrue(any("missing" in error for error in errors))
        self.assertEqual((dynamic, count), ([], 1))

    def test_nested_true_and_false_branches(self):
        errors, dynamic, count = self.scan('i18n::tr(x ? (y ? "a" : "b") : z ? "b" : "c");')
        self.assertEqual((errors, dynamic, count), ([], [], 1))

    def test_comments_and_character_literals_do_not_split_branches(self):
        source = 'i18n::tr((text[0] == \'?\' /* ?: */) ? /* x */ "a" : "b" /* : */);'
        self.assertEqual(self.scan(source), ([], [], 1))

    def test_quotes_inside_comments_cannot_hide_a_missing_branch(self):
        for comment in ("/* don't skip */", '/* say "hello */'):
            with self.subTest(comment=comment):
                errors, dynamic, count = self.scan('i18n::tr(flag ' + comment + ' ? "a" : "missing");')
                self.assertTrue(any("missing" in error for error in errors))
                self.assertEqual((dynamic, count), ([], 1))

    def test_namespace_in_nested_condition_is_not_a_branch_separator(self):
        errors, dynamic, count = self.scan('i18n::tr(outer ? ns::flag ? "a" : "b" : "missing");')
        self.assertTrue(any("missing" in error for error in errors))
        self.assertEqual((dynamic, count), ([], 1))

    def test_adjacent_literals_and_parentheses(self):
        self.assertEqual(self.scan('i18n::tr(flag ? ("a" "") : ("b"));'), ([], [], 1))

    def test_colon_and_question_mark_inside_id_are_literal(self):
        messages = {"id?:name": ("yes", "yes", "fixture")}
        self.assertEqual(self.scan('i18n::tr("id?:name");', messages), ([], [], 1))

    def test_runtime_branch_still_requires_review(self):
        for expression in ('flag ? "a" : runtimeId', 'labels[index]', 'getMessageId()', '"a" + suffix'):
            with self.subTest(expression=expression):
                errors, dynamic, count = self.scan('i18n::tr(' + expression + ');')
                self.assertEqual((errors, count), ([], 0))
                self.assertEqual(len(dynamic), 1)

    def test_format_checks_fields_for_each_branch(self):
        messages = {"a": ("路径 {path}", "Path {path}", "fixture"),
                    "b": ("失败 {error}", "Failed {error}", "fixture")}
        errors, dynamic, count = self.scan('i18n::format(flag ? "a" : "b", {{"path", name}});', messages)
        self.assertTrue(any("'b'" in error and "error" in error for error in errors))
        self.assertEqual((dynamic, count), ([], 1))

    def test_format_both_branches_with_same_fields(self):
        messages = {name: ("路径 {path}", "Path {path}", "fixture") for name in ("a", "b")}
        self.assertEqual(self.scan('i18n::format(flag() ? "a" : "b", {{"path", name}});', messages), ([], [], 1))


if __name__ == "__main__":
    unittest.main()
