from __future__ import annotations

import unittest

from repo_check_utils import strip_jsonc


class RepoCheckUtilsTest(unittest.TestCase):
    def test_strips_line_and_block_comments_without_touching_strings(self) -> None:
        source = (
            '{\n  // line\n  "url": "https://example.test/a//b",\n'
            '  /* block */ "value": 1\n}\n'
        )
        stripped = strip_jsonc(source)
        self.assertNotIn("line", stripped)
        self.assertNotIn("block", stripped)
        self.assertIn("https://example.test/a//b", stripped)


if __name__ == "__main__":
    unittest.main()
