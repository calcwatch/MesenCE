# Zstandard

This directory contains the compression portion of Zstandard 1.5.7, vendored
from https://github.com/facebook/zstd/tree/v1.5.7.

Only the single-threaded compression sources from `lib/common` and
`lib/compress`, plus `lib/zstd.h` and `lib/zstd_errors.h`, are included because
Mesen only uses single-threaded Zstandard compression. The code is built as a
static library on Windows and compiled directly into MesenCore on Linux and
macOS.

Zstandard is used under its BSD license; see `LICENSE`.
