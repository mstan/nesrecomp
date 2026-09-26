"""Verifier failure-path regressions: python -m unittest discover -s tools/cyc."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import cyc_verify


class VerifyTests(unittest.TestCase):
    def verify(self, code=0, frames=2, trace_frames=2, score=None, stale=False,
               timeout=False, missing=False, hw_difference=False, trace_difference=False):
        with tempfile.TemporaryDirectory() as tmp:
            for mode in ('oracle', 'native', 'interp'):
                if stale:
                    Path(tmp, f'rom_a0_{mode}.txt').write_text('0 trace=old hw=0\n')

            def host(cmd):
                target = Path(cmd[cmd.index('--hash-out') + 1])
                if missing:
                    return code, 'cannot load ROM'
                mode = 'oracle' if cmd[0] == 'oracle' else 'interp' if '--interp-only' in cmd else 'native'
                hw = 1 if hw_difference and mode == 'interp' else 0
                trace = 'different' if trace_difference and mode == 'native' else 'same'
                target.write_text(''.join(f'{i} trace={trace} hw={hw}\n' for i in range(trace_frames)))
                output = f'mode={mode} frames={frames} cycles=100' + (' TIMEOUT' if timeout else '') + '\n'
                if score is not None:
                    output += score + '\n'
                return code, output

            argv = ['cyc_verify.py', '--exe', 'native', '--oracle', 'oracle', '--rom', 'rom.nes',
                    '--frames', '2', '--align', '0', '--interp', '--out', tmp]
            if score is not None:
                argv += ['--acccoin']
            with patch('sys.argv', argv), patch.object(cyc_verify, 'run', side_effect=host), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                result = cyc_verify.main()
            return result, output.getvalue()

    def test_matching_complete_runs(self):
        self.assertEqual(self.verify()[0], 0)

    def test_failed_load_cannot_reuse_stale_hashes(self):
        code, output = self.verify(code=2, missing=True, stale=True)
        self.assertEqual(code, 1)
        self.assertNotIn('ALL MATCH', output)

    def test_crash_after_matching_trace(self):
        self.assertEqual(self.verify(code=3)[0], 1)

    def test_empty_matching_traces(self):
        self.assertEqual(self.verify(frames=0, trace_frames=0)[0], 1)

    def test_truncated_matching_traces(self):
        self.assertEqual(self.verify(trace_frames=1)[0], 1)

    def test_early_matching_runs(self):
        self.assertEqual(self.verify(frames=1, trace_frames=1)[0], 1)

    def test_timeout_is_not_a_match(self):
        self.assertEqual(self.verify(timeout=True)[0], 1)

    def test_documented_accuracycoin_failures_can_match(self):
        self.assertEqual(self.verify(code=1, score=
            'AccuracyCoin: 141/144 passed, 3 failed, 0 skipped, 0 not run')[0], 0)

    def test_incomplete_accuracycoin_is_not_a_match(self):
        self.assertEqual(self.verify(code=1, score=
            'AccuracyCoin: 141/144 passed, 0 failed, 0 skipped, 3 not run')[0], 1)

    def test_hardware_internals_still_compared_between_native_and_interp(self):
        self.assertEqual(self.verify(hw_difference=True)[0], 1)

    def test_trace_mismatch_still_fails(self):
        self.assertEqual(self.verify(trace_difference=True)[0], 1)


if __name__ == '__main__':
    unittest.main()
