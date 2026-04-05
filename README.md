# alpbook

**A fast order book and trading system implementation in C++ 23.**

alpbook is my C++ 23 modules order book library and NASDAQ trading system
implementation built for high-throughput, low-latency market data processing
with strong error recovery.

There are two components. alpbook provides a more generic order book with user-defined,
generalizable strategies. alpdaq, a subcomponent, is a runtime harness that handles an
ITCH event loop that provides data to user-defined strategies.

On the alpbook front, here are a few key design decisions:

- **Two storage policies** — `PolicyHash` uses a hash-map per price level for
  O(1) best-bid/ask tracking; `PolicyTree` uses a augmented B+ tree for O(log n)
  cumulative-volume queries (`getBuyVolumeAhead`, `getSellVolumeAhead`).
- **An extensible yet performant interface** - `Book`, `Listener`, `Strategy`, and parser callbacks are
  each expressed through C++ 20 concepts for modularity and to reduce branching.

alpdaq is perhaps more interesting. Like alpbook, it is very extensible at zero-cost: a `System` can
be used via a network connection or a file connection, or with any strategy or logger.

The default implementation provides:

- The `System` class (obviously). Handles the ITCH state machine throughout the various phases of a session,
  while also managing error and gap recovery, halts, and stock mappings.
- A lock-free, asynchronous logging system `Logger` run on a separate pinned thread.

Note: this is a personal exploration of trading systems and is not tested for non-simulated environments (yet).

See my other project [avalanche](https://github.com/benaepli/avalanche) for a Rust simulator of ITCH network traffic
that is intended to be paired with this project.

## Building from Source

### Requirements

- **C++23 compiler**: Clang 16+, GCC 14+, or MSVC 19.34+
- **CMake**: 3.28+ (3.30+ recommended for best module support)
- **Ninja**

### Quick Build

```bash
cmake -B build -S . -G Ninja
cmake --build build
```

For detailed instructions, compiler configuration, and troubleshooting,
see [docs/building.md](docs/building.md).

## Documentation

- **API Documentation**: https://benaepli.github.io/alpbook/
- **Design Overview**: https://benaepli.github.io/alpbook/design.html
- **Building Guide**: [docs/building.md](docs/building.md)

## Benchmarks

The [benchmarks](benchmarks/) folder contains latency tests for `alpdaq`. These benchmarks measure
userspace end-to-end processing time of ITCH messages, from the system's initial `poll` call to the corresponding book
event being delivered to a strategy.

### Conditions

I ran these benchmarks on my Framework laptop with a Ryzen 7 7840U on Fedora 42.

In this test, I listened to two stocks: `AAPL` and `GOOGL` in the default benchmark
configuration.

### Latency

| Percentile | Latency (ns) |
|------------|--------------|
| p50        | 110          |
| p90        | 270          |
| p95        | 370          |
| p99        | 701          | 

## Dependencies

All dependencies are fetched either locally or with `FetchContent`.

| Dependency                                                           | Version    | Purpose                                                |
|----------------------------------------------------------------------|------------|--------------------------------------------------------|
| [abseil-cpp](https://github.com/abseil/abseil-cpp)                   | 20240116.2 | `btree_set`, and `uint128` for price/qty types         |
| [toml++](https://github.com/marzer/tomlplusplus)                     | 3.4.0      | TOML configuration parsing                             |
| [spdlog](https://github.com/gabime/spdlog)                           | 1.17.0     | Logging backend (with `std::format`)                   |
| [hwloc](https://github.com/open-mpi/hwloc)                           | 2.12.0     | CPU topology discovery and thread pinning              |
| [readerwriterqueue](https://github.com/cameron314/readerwriterqueue) | 1.0.6      | Lock-free SPSC queue for async log dispatch            |
| [zlib](https://github.com/madler/zlib)                               | 1.3.1      | Gzip decompression of ITCH feed files                  |
| [BppTree](deps/BppTree)                                              | local      | Augmented B+ tree used by `PolicyTree` storage backend |
| [tscns](deps/tscns)                                                  | local      | TSC-to-nanosecond calibration for latency benchmarks   |
| [GTest](https://github.com/google/googletest)                        | latest     | Unit test framework                                    |
| [GBench](https://github.com/google/benchmark)                        | 1.9.4      | Micro-benchmark framework for isolated book operations |

## License

MIT License - Copyright (c) 2026 Ben Aepli
