# Market data

**No Nasdaq data is committed to this repository**, neither raw files nor derived slices.
Everything under `data/` except this README and `checksums.sha256` is git-ignored.

## Source

NASDAQ publishes sample TotalView-ITCH 5.0 day files at
<https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/> (location verified 2026-10-04). The listing
page shows no terms-of-use text; the files are used here only locally, for research and
education, and are never redistributed.

| File | Size (bytes) | Role |
|---|---|---|
| `07302019.NASDAQ_ITCH50.gz` | 3 662 140 094 | primary day for E1–E6 |

The `.md5sum` files shown in the listing are not downloadable (the server returns 404), so
integrity is checked by exact byte size and `gzip -t`, and the file is then pinned by the
SHA-256 in `checksums.sha256`.

## Fetch

```bash
scripts/fetch_itch.sh            # downloads (resumable), verifies size, gzip integrity, sha256
```

## Format (summary; see docs/itch-moldudp64.md)

The decompressed file is a sequence of ITCH 5.0 messages, each prefixed by a 2-byte
big-endian length. All integer fields are big-endian; prices are 4-byte integers with 4
implied decimal places; timestamps are 6-byte nanoseconds since midnight.

The day file is several GB compressed and much larger uncompressed, so it is always
**streamed** (`pigz -dc` or zlib), never loaded into memory (the dev machine has 7.6 GiB RAM).
