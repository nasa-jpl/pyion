import threading
import time
import os
import unittest
import sys
from unittest.mock import MagicMock, patch

# --- MOCK INITIALIZATION ---
class UniqueMock(MagicMock):
    _counter = 1000
    def __getattr__(self, name):
        if name.startswith('_'):
            return super().__getattr__(name)
        # Constants in pyion are usually CamelCase or ALL_UPPER
        # Methods are usually snake_case
        if name[0].isupper():
            val = UniqueMock._counter
            UniqueMock._counter += 1
            setattr(self, name, val)
            return val
        return super().__getattr__(name)

mock_bp = UniqueMock()
mock_cfdp = UniqueMock()
sys.modules['_bp'] = mock_bp
sys.modules['_ltp'] = UniqueMock()
sys.modules['_cfdp'] = mock_cfdp
sys.modules['_mgmt'] = UniqueMock()
sys.modules['_mem'] = UniqueMock()
sys.modules['_utils'] = UniqueMock()

# Now we can import pyion
import pyion
from pyion.utils import in_ion_folder, ion_dir_lock

class TestConcurrency(unittest.TestCase):
    
    def setUp(self):
        print(f"\nRunning: {self._testMethodName}")

    def test_directory_lock(self):
        """Verify that in_ion_folder uses the global lock to serialize directory changes."""
        mock_self = MagicMock()
        mock_self.node_dir = MagicMock()
        mock_self.node_dir.absolute.return_value = "/tmp/node1"
        
        call_order = []
        def mocked_chdir(path):
            call_order.append(f"chdir {path}")
            if path == "/tmp/node1":
                time.sleep(0.1)

        @in_ion_folder
        def dummy_func(self):
            return "ok"

        with patch('os.chdir', side_effect=mocked_chdir), \
             patch('os.getcwd', return_value="/original"):
            
            t1 = threading.Thread(target=lambda: dummy_func(mock_self))
            t2 = threading.Thread(target=lambda: dummy_func(mock_self))
            
            t1.start()
            t2.start()
            t1.join()
            t2.join()
            
            try:
                self.assertEqual(call_order[0], "chdir /tmp/node1")
                self.assertEqual(call_order[1], "chdir /original")
                self.assertEqual(call_order[2], "chdir /tmp/node1")
                self.assertEqual(call_order[3], "chdir /original")
                print("  [PASS] Directory changes were correctly serialized.")
            except (AssertionError, IndexError):
                print(f"  [FAIL] Sequence was: {call_order}")
                raise

    def test_endpoint_serialization(self):
        """Verify that concurrent calls on the same endpoint are serialized by the instance lock."""
        from pyion.bp import Endpoint
        from pyion.constants import BpCustodyEnum
        
        mock_proxy = MagicMock()
        mock_proxy.node_dir = None
        
        # Create endpoint with proper custody constant
        no_custody = int(BpCustodyEnum.NO_CUSTODY_REQUESTED)
        ept = Endpoint(mock_proxy, "ipn:1.1", 0x1234, 3600, 1, None, no_custody, 0, 0, 0, False, None, None, False, False)
        
        call_log = []
        def mocked_bp_send(*args):
            call_log.append("start_send")
            time.sleep(0.3)
            call_log.append("end_send")
        
        mock_bp.bp_send.side_effect = mocked_bp_send
        mock_bp.bp_receive.return_value = b"data"
        
        # t1 will hold the lock for 0.3 seconds
        t1 = threading.Thread(target=lambda: ept.bp_send("ipn:2.1", b"hello"))
        
        t1.start()
        time.sleep(0.1) # Ensure t1 has acquired the lock
        
        start_wait = time.time()
        # This call should block until t1 finishes
        ept.bp_receive()
        end_wait = time.time()
        
        t1.join()
        
        wait_duration = end_wait - start_wait
        
        try:
            self.assertGreater(wait_duration, 0.15)
            self.assertEqual(call_log[0], "start_send")
            self.assertEqual(call_log[1], "end_send")
            print(f"  [PASS] bp_receive blocked for {wait_duration:.2f}s as expected.")
        except AssertionError:
            print(f"  [FAIL] bp_receive did not block! Serialization failed. Wait was only {wait_duration:.2f}s.")
            raise

if __name__ == '__main__':
    print("="*60)
    print("PYION CONCURRENCY REGRESSION TEST")
    print("="*60)
    
    suite = unittest.TestLoader().loadTestsFromTestCase(TestConcurrency)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    
    if result.wasSuccessful():
        print("\nOVERALL STATUS: PASSED")
        sys.exit(0)
    else:
        print("\nOVERALL STATUS: FAILED")
        sys.exit(1)
