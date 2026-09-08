#!/usr/bin/env python3
"""Computes the cross-language proto_hash contract at build time.

Authoritative algorithm (confirmed against the real implementation, not
just prose -- Session-Manager/src/session_manager/ipc/handshake.py:
compute_proto_hash(), which is what actually runs at the UDS transport
handshake and rejects a mismatch, per docs/ANSWERS_FROM_C.md §5):

  1. Every file matching *.proto directly in the contract directory (no
     recursion).
  2. Sorted lexicographically, plain byte-wise ASCII sort.
  3. Raw bytes of each file, in that order -- no decoding, no comment
     stripping, no whitespace normalization.
  4. Fed into a single BLAKE3-256 hash, no separator between files.
  5. The digest is the standard 32-byte BLAKE3 output -- NOT hex-encoded
     on the wire (rx.proto's ReceiverHello.proto_hash is `bytes`, and
     handshake.py's compute_proto_hash() returns hasher.digest(), the raw
     32 bytes, not .hexdigest()).

Run directly against sender/src/common/proto_hash.hpp.in's own CMake
recipe wasn't available to copy verbatim (docs/ANSWERS_FROM_A.md §13
recommended that, but sender/'s current accessible checkout is still at
commit fb6be44 -- the newer commits A described pushing haven't reached
this local clone; `git fetch` here shows no new history). So this script
was written directly against session_manager's real, verified
handshake.py instead, which is the more authoritative source in any case
-- it's what the UDS handshake actually compares against
(ipc/uds.py:_complete_handshake -> handshake.verify_proto_hash()).

Usage: proto_hash.py <path1> <path2> ...
  Paths are hashed in the exact order given -- CMakeLists.txt is
  responsible for sorting them first (list(SORT ...)), matching
  handshake.py's sorted(proto_dir.glob("*.proto")).

Output: the 32-byte digest as a comma-separated list of "0xHH" tokens on
one line, e.g. "0x1a, 0x2b, ...", suitable for splatting directly into a
C++ array initializer via configure_file().
"""

import sys

try:
    import blake3
except ImportError:
    sys.stderr.write(
        "proto_hash.py: the 'blake3' Python package is required at build "
        "time (never linked into the built binary -- see the module "
        "docstring). Install it, e.g.:\n"
        "  pip install blake3\n"
    )
    sys.exit(1)


def main() -> int:
    paths = sys.argv[1:]
    if not paths:
        sys.stderr.write("proto_hash.py: no .proto files given\n")
        return 1

    hasher = blake3.blake3()
    for path in paths:
        with open(path, "rb") as f:
            hasher.update(f.read())

    digest = hasher.digest()
    if len(digest) != 32:
        sys.stderr.write(f"proto_hash.py: expected a 32-byte digest, got {len(digest)}\n")
        return 1

    print(", ".join(f"0x{b:02x}" for b in digest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
