import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from bund.syncbridge import iter_sync  # noqa: E402


class TestBridge(unittest.TestCase):
    def test_items_in_order(self):
        async def gen():
            for i in range(5):
                yield i
        self.assertEqual(list(iter_sync(gen())), [0, 1, 2, 3, 4])

    def test_exception_relayed(self):
        async def gen():
            yield 1
            raise ValueError("boom")
        it = iter_sync(gen())
        self.assertEqual(next(it), 1)
        with self.assertRaises(ValueError):
            next(it)


if __name__ == "__main__":
    unittest.main()
