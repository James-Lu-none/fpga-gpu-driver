"""Run the C CSR tool against a file-backed BAR0, without FPGA/root access."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

class DebugReaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        cls.tool = Path(cls.build.name) / 'test_csr'
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(Path(__file__).with_name('test_csr.c')), '-o', str(cls.tool)], check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.device = Path(self.temp.name)
        self.image = bytearray(0x100000)
        self.put(0x20010, 0x47505544)
        self.put(0x20014, 1)
        self.put(0x20018, 0xB)  # DDR/PCIe ready while CPU remains in reset.

    def put(self, offset, value):
        struct.pack_into('<I', self.image, offset, value)

    def run_reader(self, *options):
        (self.device / 'resource0').write_bytes(self.image)
        result = subprocess.run([str(self.tool), '--json', str(self.device / 'debug.json'),
                                 *options, str(self.device / 'resource0')], capture_output=True, text=True)
        self.assertEqual((self.device / 'resource0').read_bytes(), self.image)
        return result

    def snapshot(self):
        result = self.run_reader()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('[DEBUG:manual]', result.stdout)
        return json.loads((self.device / 'debug.json').read_text())

    def test_decode_without_firmware_or_mmio_writes(self):
        self.put(0x2001C, 0x10)
        self.put(0x30030, 1618)
        self.put(0x30078, 32)
        self.put(0x30080, 15)
        args = [0x10000, 0x10000, 0x11000, 0x11000, 0x12000, 0x12000, 64, 64]
        for i, value in enumerate(args):
            self.put(0x30088 + i*4, value)
        self.put(0x300A8, 0x12000)
        self.put(0x300AC, 0xF)
        self.put(0x300B0, 13)
        result = self.snapshot()
        self.assertTrue(result['system']['mig_calib_done'])
        self.assertFalse(result['system']['cpu_running'])
        self.assertEqual(result['faults'], ['axi_read_error'])
        self.assertEqual(result['counters']['cycles'], 1618)
        self.assertEqual(result['counters']['warp_done'], 32)
        self.assertEqual(result['axi']['first_read_words'], args)
        self.assertEqual(result['axi']['first_write_address'], 0x12000)
        self.assertEqual(result['axi']['first_write_words'], [13, 0])

    def test_unseen_payload_is_not_reported_as_valid(self):
        self.put(0x30088, 0xDEADBEEF)
        result = self.snapshot()
        self.assertIsNone(result['axi']['first_read_words'])
        self.assertIsNone(result['axi']['first_write_address'])

    def test_old_bitstream_is_rejected(self):
        self.put(0x20010, 0)
        result = self.run_reader()
        self.assertEqual(result.returncode, 1)
        self.assertIn('rebuild/program', result.stderr)

    def test_ready_gate_preserves_diagnostics(self):
        self.put(0x20018, 8)
        result = self.run_reader('--require-ready')
        self.assertEqual(result.returncode, 1)
        self.assertIn('DUT is not ready', result.stderr)
        self.assertEqual(json.loads((self.device / 'debug.json').read_text())['status_raw'], 8)

    def test_small_bar_is_rejected_before_mapping(self):
        self.image = bytearray(4096)
        result = self.run_reader()
        self.assertEqual(result.returncode, 1)
        self.assertIn('too small', result.stderr)


if __name__ == '__main__':
    unittest.main()
