# Durable Journal

A small C++17 append-only binary journal with checksummed frames, monotonic sequence numbers, OS-level exclusive ownership and explicit recovery of an incomplete final write. Header-only, no third-party dependencies, Windows/Linux/macOS.

## Why it exists

Appending a line to a text file does not tell a reader whether the final write finished. This journal frames each record, checks its payload and sequence, and refuses damaged data. Recovery truncates only an incomplete final frame when explicitly requested; checksum/header corruption is an error.

```cpp
#include <journal/journal.hpp>
journal::log events("events.bin");
auto sequence = events.append("job completed");
events.replay([](std::uint64_t id, std::string_view bytes) {
    // Process one record; the view lives only for this callback.
});
```

## Build and try

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/journal_cli events.bin append "first event"
./build/journal_cli events.bin replay
./build/journal_cli events.bin repair
```

Visual Studio places binaries under `build/Release/`. The `repair` command is explicit and destructive to an incomplete tail: back up important logs first. It prints the byte count removed.

## Format

All integers are little-endian. Each frame consists of a 24-byte header followed by 0–16 MiB of opaque payload.

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | Magic/version DJ01 |
| 4 | 4 | Payload length |
| 8 | 8 | Sequence, starting at 1 |
| 16 | 4 | IEEE CRC-32 over header bytes 0–15 and payload |
| 20 | 4 | Reserved, zero |

An append acknowledges only after `fsync` (POSIX) or `FlushFileBuffers` (Windows) succeeds. A write error poisons that instance; reopen to inspect/recover it. Replay uses memory bounded by one record. Opening validates the entire file, so startup is O(file size).

## Contract and limits

- One owner per file; POSIX advisory `flock` or exclusive Windows handle. Other programs that ignore advisory locking can still corrupt a POSIX file.
- Single-threaded API; callers must synchronize access. Replay callbacks must not call the journal again.
- No transactional groups, compaction, indexes, replication or encryption. CRC detects accidental damage, not malicious modification.
- Durability depends on OS/filesystem/hardware guarantees. Parent-directory persistence for a newly created file is not guaranteed. The tests simulate truncated writes, not real power cuts.
- Incomplete-tail repair cannot distinguish a genuinely interrupted write from every possible header/length corruption. Back up before recovery. Fully available corrupt records are never silently discarded.
- Local regular files are the intended storage. Network filesystems and non-cooperating writers are outside the contract.

Tests cover binary and empty payloads, reopen/sequence behavior, exclusive ownership, every truncation boundary of the last sample record and rejection of checksum or magic corruption. CI checks Debug and Release on Linux, Windows and macOS.

MIT license.
