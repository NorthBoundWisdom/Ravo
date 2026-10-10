import tempfile
import unittest
from pathlib import Path

from check_ccache import parse_stats, read_cache, verify


class CompilerCacheTests(unittest.TestCase):
    def test_miss_and_warm_hit_are_both_real_compilations(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root)
            cache_file = path / "CMakeCache.txt"
            cache_file.write_text(
                "// cache\nCMAKE_C_COMPILER_LAUNCHER:STRING=ccache\n"
                "CMAKE_CXX_COMPILER_LAUNCHER:UNINITIALIZED=C:/tools/ccache.exe\n",
                encoding="utf-8",
            )
            cache = read_cache(cache_file)
            for text, hits in [
                ("direct_cache_hit\t0\npreprocessed_cache_hit\t0\ncache_miss\t1\n", 0),
                ("direct_cache_hit\t1\npreprocessed_cache_hit\t0\ncache_miss\t0\n", 1),
            ]:
                result = verify(path, root, cache, parse_stats(text))
                self.assertEqual(result["cacheable_calls"], 1)
                self.assertEqual(result["direct_hits"], hits)
            stats = parse_stats(
                "direct_cache_hit\t0\npreprocessed_cache_hit\t0\ncache_miss\t0\n"
            )
            with self.assertRaisesRegex(ValueError, "no cacheable compilations"):
                verify(path, root, cache, stats)
            with self.assertRaisesRegex(ValueError, "directory mismatch"):
                verify(path, str(path / "other"), cache, stats)
            cache["CMAKE_CXX_COMPILER_LAUNCHER"] = ""
            with self.assertRaisesRegex(ValueError, "must invoke ccache"):
                verify(path, root, cache, stats)


if __name__ == "__main__":
    unittest.main()
