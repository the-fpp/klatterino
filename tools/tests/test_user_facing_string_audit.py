import unittest

from tools.user_facing_string_audit import (
    InventoryEntry,
    disposition,
    feature_area,
    justification,
    markdown,
    parse_diff,
    review_class,
)

class UserFacingStringAuditTests(unittest.TestCase):
    def test_parse_diff_tracks_added_and_removed_quoted_lines(self):
        diff = """diff --git a/a.cpp b/a.cpp
--- a/a.cpp
+++ b/a.cpp
@@ -5,2 +5,2 @@
-QStringLiteral(\"Old UI text\");
+QStringLiteral(\"New UI text\");
 context();
"""

        self.assertEqual(
            parse_diff(diff),
            [
                InventoryEntry(
                    "a.cpp", 5, "removed", 'QStringLiteral("Old UI text");'
                ),
                InventoryEntry(
                    "a.cpp", 5, "added", 'QStringLiteral("New UI text");'
                ),
            ],
        )

    def test_markdown_escapes_table_and_code_delimiters(self):
        report = markdown(
            [
                InventoryEntry(
                    "deps/chatterino7/src/Application.cpp",
                    4,
                    "added",
                    'QStringLiteral("a|`b")',
                )
            ],
            "base",
            "head",
        )

        self.assertIn("Candidates: **1**", report)
        self.assertIn("retain: 1", report)
        self.assertIn("application: 1", report)
        self.assertIn("implementation or diagnostic literal", report)
        self.assertIn("Review coverage by feature area", report)
        self.assertIn("a\\|\\`b", report)

    def test_dispositions_and_feature_areas_are_stable(self):
        self.assertEqual(
            disposition(InventoryEntry("a.cpp", 1, "added", '"new"')),
            "retain",
        )
        self.assertEqual(
            disposition(InventoryEntry("a.cpp", 1, "removed", '"old"')),
            "remove",
        )
        self.assertEqual(
            feature_area("deps/chatterino7/src/providers/rumble/RumbleApi.cpp"),
            "provider: rumble",
        )
        ui_entry = InventoryEntry(
            "deps/chatterino7/src/widgets/dialogs/LoginDialog.cpp", 1, "added", '"x"'
        )
        self.assertEqual(review_class(ui_entry), "normal UI copy")
        self.assertEqual(
            justification(ui_entry), "Retained as reviewed user-facing copy."
        )

    def test_parse_diff_excludes_preprocessor_includes(self):
        diff = """diff --git a/a.cpp b/a.cpp
--- a/a.cpp
+++ b/a.cpp
@@ -1,1 +1,2 @@
+#include \"Widget.hpp\"
+QStringLiteral(\"Visible text\");
"""

        self.assertEqual(
            parse_diff(diff),
            [InventoryEntry("a.cpp", 2, "added", 'QStringLiteral("Visible text");')],
        )


if __name__ == "__main__":
    unittest.main()
