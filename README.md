# LogMinify

LogMinify is a lossless, parsing-free log compressor targeted at log streams. Specifically, it establishes a global dictionary across log blocks to eliminate the cross-block repetition with a learned index, LogSpline, to accelerate dictionary updates. 

## Requirements

- Linux (x86-64), g++ >= 9 (C++17)
- libpcre2-dev, libjemalloc-dev
- tar with lzma/xz support

## Build

```bash
make
```

This produces `./logminify` and `./decompress_logminify`.

## Input layout

```text
Logs/<name>/<name>.log
```

The built-in token and timestamp rules cover the 16 LogHub datasets: Android, Apache,
BGL, Hadoop, HDFS, HealthApp, HPC, Linux, Mac, OpenSSH, OpenStack, Proxifier, Spark,
Thunderbird, Windows, and Zookeeper. For another log format, add its rules to the
`LogProcessor` constructor in `src/logminify.cpp`.

## Compress

```bash
mkdir -p output temp
LOGMINIFY_FG_THREADS=40 ./logminify <name> 100000
```

The second argument is the block size in lines (100000 in the paper's experiments).

| Variable | Default | Meaning |
| --- | --- | --- |
| `LOGMINIFY_FG_THREADS` | hardware concurrency | number of blocks compressed in parallel |
| `LOGMINIFY_COMPRESS` | `lzma -6` | backend compressor for the block and dictionary archives, e.g. `gzip -6`, `bzip2 -3`, `lzma -9` |

Outputs under `output/<name>/`:

- `compressed<id>.tar.xz` - one archive per block
- `rsspdc.tar.xz` - the global template dictionary
- `noeol` - an empty marker present when the input's last line has no trailing newline

`info.txt` records the original, block-archive and dictionary sizes, the compression
ratio (original / (blocks + dictionary)), the compression speed (original MB / wall
time), and the index build time.

A run removes `output/<name>/*` and `temp/<name>/*` first.

## Decompress

```bash
LOGMINIFY_DEC_THREADS=40 ./decompress_logminify <name>
```

This writes `output/<name>_decompressed.log`. The default thread count is the
hardware concurrency. Only the block archives and `rsspdc.tar.xz` are needed; the
decompressor does not use the compressor's runtime state.

## Verify lossless

```bash
python3 scripts/verify_roundtrip.py <name>
```

The script runs the decompressor and compares the SHA-256 of the result with the
input, printing `PASS` or `FAIL` and exiting non-zero on a mismatch.

## Example

```bash
mkdir -p Logs/Linux output temp
cp /path/to/Linux.log Logs/Linux/Linux.log
LOGMINIFY_FG_THREADS=8 ./logminify Linux 100000
python3 scripts/verify_roundtrip.py Linux
```

## Notes

- The build uses `-march=native`; rebuild on the machine where you run it.
- Building with `-DLOGMINIFY_PROFILE` adds per-phase timing counters (`src/profile.h`).
- The compressor expects to run from the repository root (it uses `Logs/`, `output/`
  and `temp/` relative to the working directory).

## License

MIT; see `LICENSE`. The headers under `logspline/rs` derive from RadixSpline
(MIT); see `THIRD_PARTY_NOTICES.md`.
