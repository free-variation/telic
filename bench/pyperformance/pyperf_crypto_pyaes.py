"""
AES-128 CTR benchmark, the reference for crypto-pyaes.telic. The key and cleartext
are pyperformance bm_crypto_pyaes's, and each loop encrypts then decrypts with a
fresh CTR object as that benchmark does; the telic port uses the same two values,
so the comparison and the checksum line up. Uses the pure-Python `pyaes` module
(pip install pyaes), as bm_crypto_pyaes does.
"""
import sys
import time as _t

import pyaes

KEY = b'\xa1\xf6%\x8c\x87}_\xcd\x89dHE8\xbf\xc9,'
MSG = b"This is a test. What could possibly go wrong? " * 500
NBYTES = len(MSG)


def encrypt(data):
    # default Counter starts at 1, matching the .telic (counter = block index + 1)
    return pyaes.AESModeOfOperationCTR(KEY).encrypt(data)


def decrypt(data):
    return pyaes.AESModeOfOperationCTR(KEY).decrypt(data)


if __name__ == "__main__":
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    ct = back = b""
    t0 = _t.perf_counter()
    for _ in range(loops):
        ct = encrypt(MSG)
        back = decrypt(ct)
    elapsed = _t.perf_counter() - t0
    print(f"roundtrip ok: {1 if back == MSG else 0}")
    print(f"checksum: {sum(ct)}")
    print(f"elapsed: {elapsed:.6f} s ({NBYTES} bytes x {loops})")
