"""Small rails for ASLR mapping and rejecting unidentified historical samples."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "attribute_host_samples", Path(__file__).parents[1] / "tools" / "attribute_host_samples.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class AttributionTests(unittest.TestCase):
    def test_relocation_boundaries_and_external_addresses(self):
        samples = "# module_base=7ff00000 module_size=8192\n# sample_errors=0 child_exit=0\n7ff00100,2\n7ff001ff,3\n7ff00200,5\n7fefffff,7\n7ff02000,11\n"
        symbols = "140000100 T first\n140000200 t second\n140000100 t .text\n"
        result = module.attribute(samples, symbols, 0x140000000)
        counts = {item["function"]: item["samples"] for item in result["functions"]}
        self.assertEqual(counts, {"first": 5, "second": 5, "outside-executable": 18})
        self.assertEqual(result["samples"], 28)

    def test_missing_module_identity_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "identity"):
            module.attribute("7ff00100,5", "140000100 T first", 0x140000000)

    def test_failed_empty_or_mismatched_capture_is_rejected(self):
        header = "# module_base=7ff00000 module_size=8192\n# sample_errors=0 child_exit=0\n"
        symbols = "140000100 T first"
        for samples, preferred in [(header, 0x140000000),
                                   (header.replace("sample_errors=0", "sample_errors=1") + "7ff00100,1", 0x140000000),
                                   (header.replace("child_exit=0", "child_exit=7") + "7ff00100,1", 0x140000000),
                                   (header + "7ff00100,1", 0x400000)]:
            with self.subTest(samples=samples, preferred=preferred):
                with self.assertRaises(ValueError):
                    module.attribute(samples, symbols, preferred)


if __name__ == "__main__":
    unittest.main()
