#!/usr/bin/env python3
"""Real pipe/protocol tests with an independent native-library boundary fixture."""
from pathlib import Path
import struct
import tempfile
import unittest

from native_decryption import MAX_FRAME, NativeDecryption, sample_sizes
from web_api import ApiError


def batch(samples):
    return struct.pack(">I", len(samples)) + b"".join(struct.pack(">I", len(s)) + s for s in samples)


class NativeDecryptionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name)
        self.binary = root / "worker"
        self.binary.write_text('''#!/usr/bin/python3
import struct, sys
def read(n):
    result = sys.stdin.buffer.read(n)
    if len(result) != n: raise EOFError()
    return result
def write(data, ok=True):
    value = bytes([0 if ok else 1]) + data
    sys.stdout.buffer.write(struct.pack('>I',len(value))+value)
    sys.stdout.buffer.flush()
write(b'NAD2')
handles = 0
key_cache = {}
handle_masks = {}
while True:
    try: data=read(struct.unpack('>I',read(4))[0])
    except EOFError: break
    op=struct.unpack_from('>I',data)[0]
    if op == 1:
        offset=4
        def string():
            global offset
            size=struct.unpack_from('>I',data,offset)[0];offset+=4
            value=data[offset:offset+size];offset+=size
            return value
        adam,uri=string(),string()
        if uri.endswith(b'reject'):
            write(b'',False); continue
        handles+=1
        # Independent library boundary: FootHill caches by catalog asset alone.
        # Two renditions of one asset need a reset to retrieve different keys.
        mask=key_cache.setdefault(adam,0xbb if uri.endswith(b'/aac') else 0xaa)
        handle_masks[handles]=mask
        key=b'fixture-persistent-key-'+str(mask).encode()
        write(struct.pack('>II',handles,len(key))+key)
    elif op == 3:
        key_cache.clear()
        write(b'')
    elif op == 2:
        handle,count=struct.unpack_from('>II',data,4)
        offset=12
        result=struct.pack('>I',count)
        for _ in range(count):
            size=struct.unpack_from('>I',data,offset)[0];offset+=4
            sample=data[offset:offset+size];offset+=size
            head=size//16*16
            clear=bytes(v^handle_masks[handle] for v in sample[:head])+sample[head:]
            result+=struct.pack('>I',size)+clear
        write(result)
''')
        self.binary.chmod(0o700)
        self.backend = NativeDecryption(self.binary, root, root)

    def tearDown(self):
        self.backend.close()
        self.directory.cleanup()

    def test_reuses_worker_and_context_without_changing_sample_boundaries_or_tails(self):
        first = self.backend.context("123", "skd://fixture")
        process = self.backend.process
        second = self.backend.context("123", "skd://fixture")
        self.assertEqual(first, second)
        self.assertEqual(self.backend.opens, 1)
        self.assertIs(process, self.backend.process)
        samples = [bytes(range(35)), b"short", bytes(range(16))]
        expected = [bytes(v ^ 0xaa for v in s[:len(s)//16*16]) + s[len(s)//16*16:] for s in samples]
        for _ in range(2):
            self.assertEqual(self.backend.decrypt(first["nativeContext"], batch(samples)), batch(expected))
        self.assertIs(process, self.backend.process)

    def test_worker_exit_rehydrates_existing_context(self):
        context = self.backend.context("123", "skd://fixture")
        old = self.backend.process
        old.kill(); old.wait(timeout=2)
        sample = bytes(range(32))
        self.assertEqual(self.backend.decrypt(context["nativeContext"], batch([sample])),
                         batch([bytes(v ^ 0xaa for v in sample)]))
        self.assertNotEqual(old.pid, self.backend.process.pid)

    def test_same_song_rendition_keys_do_not_alias_and_switching_back_stays_valid(self):
        alac = self.backend.context("123", "skd://fixture/alac")
        aac = self.backend.context("123", "skd://fixture/aac")
        self.assertNotEqual(alac["contentKey"], aac["contentKey"])
        sample = bytes(range(35))
        for context, mask in [(alac, 0xaa), (aac, 0xbb), (alac, 0xaa)]:
            expected = bytes(value ^ mask for value in sample[:32]) + sample[32:]
            self.assertEqual(self.backend.decrypt(context["nativeContext"], batch([sample])), batch([expected]))

    def test_context_storage_and_library_context_lifetimes_are_bounded(self):
        contexts = [self.backend.context(str(n+1), "skd://fixture/"+str(n)) for n in range(66)]
        self.assertEqual(len(self.backend.contexts), 32)
        self.assertEqual(self.backend.generation, 2)
        sample = b"short"
        self.assertEqual(self.backend.decrypt(contexts[-3]["nativeContext"], batch([sample])), batch([sample]))
        with self.assertRaises(ApiError) as error:
            self.backend.decrypt(contexts[0]["nativeContext"], batch([sample]))
        self.assertEqual(error.exception.code, 409)

    def test_bad_or_expired_context_cannot_start_native_worker(self):
        for token in ["", "bad", "a"*48]:
            with self.subTest(token=token), self.assertRaises(ApiError) as error:
                self.backend.decrypt(token, batch([b"sample"]))
            self.assertEqual(error.exception.code, 409)
        self.assertIsNone(self.backend.process)

    def test_malformed_batches_are_rejected_before_native_execution(self):
        invalid = [b"", batch([]), batch([b""]), batch([b"a"])+b"tail",
                   struct.pack(">II", 1, 200)+b"short", struct.pack(">I", 8193), b"x"*(MAX_FRAME+1)]
        for value in invalid:
            with self.subTest(length=len(value)), self.assertRaises(ApiError) as error:
                self.backend.decrypt("a"*48, value)
            self.assertEqual(error.exception.code, 400)
        self.assertIsNone(self.backend.process)

    def test_failed_key_is_not_retained_and_shutdown_reaps_worker(self):
        with self.assertRaises(ApiError) as error:
            self.backend.context("123", "skd://reject")
        self.assertEqual(error.exception.code, 422)
        self.assertEqual(len(self.backend.contexts), 0)
        process = self.backend.process
        self.backend.close()
        self.assertIsNotNone(process.poll())
        with self.assertRaises(ApiError) as error:
            self.backend.context("123", "skd://fixture")
        self.assertEqual(error.exception.code, 503)

    def test_independent_frame_validator_checks_all_lengths(self):
        self.assertEqual(sample_sizes(batch([b"one", b"two"])), [3, 3])


if __name__ == "__main__":
    unittest.main(verbosity=2)
