#!/usr/bin/env python3
"""Decompress a dataset and check that the result is byte-identical to the input.

Usage:
    python3 scripts/verify_roundtrip.py <logname> [--skip-decompress]

Run from the repository root, where Logs/ and output/ live (the paths are
configurable). Exits 0 on a byte-identical round trip and 1 otherwise.
"""
import argparse
import hashlib
import os
import subprocess
import sys


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logname")
    parser.add_argument("--decompressor", default="./decompress_logminify")
    parser.add_argument("--logs", default="Logs")
    parser.add_argument("--output", default="output")
    parser.add_argument("--skip-decompress", action="store_true",
                        help="compare the existing decompressed file only")
    args = parser.parse_args()

    original = os.path.join(args.logs, args.logname, args.logname + ".log")
    restored = os.path.join(args.output, args.logname + "_decompressed.log")
    if not os.path.isfile(original):
        print("FAIL: input not found: " + original)
        return 1

    if not args.skip_decompress:
        result = subprocess.run([args.decompressor, args.logname])
        if result.returncode != 0:
            print("FAIL: decompressor exited with %d" % result.returncode)
            return 1

    if not os.path.isfile(restored):
        print("FAIL: decompressed file not found: " + restored)
        return 1

    original_size = os.path.getsize(original)
    restored_size = os.path.getsize(restored)
    original_hash = sha256(original)
    restored_hash = sha256(restored)
    if original_size == restored_size and original_hash == restored_hash:
        print("PASS: %s (%d bytes, sha256 %s)" % (args.logname, original_size, original_hash))
        return 0
    print("FAIL: %s differs" % args.logname)
    print("  original:   %d bytes, sha256 %s" % (original_size, original_hash))
    print("  decompressed: %d bytes, sha256 %s" % (restored_size, restored_hash))
    return 1


if __name__ == "__main__":
    sys.exit(main())
