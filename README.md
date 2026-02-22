# alpbook

**A fast, extensible order book library in C++23**

alpbook is my C++23-modules order book library built for
high-throughput, low-latency market data processing. It parses and replays
NASDAQ ITCH 5.0 feed data, maintains per-symbol order books, and delivers
top-of-book and trade events to user-defined strategies.

Key design decisions:

- **Two storage policies** — `PolicyHash` uses a hash-map per price level for
  O(1) best-bid/ask tracking; `PolicyTree` uses a augmented B+ tree for O(log n)
  cumulative-volume queries (`getBuyVolumeAhead`, `getSellVolumeAhead`).
- **Concept-based interfaces** — `Book`, `Listener`, `Strategy`, and
  `StrategyFactory` are expressed as C++20 concepts, so any conforming type
  plugs in with zero virtual dispatch.
- **Async and synchronous dispatch** — `Dispatcher<Slot, Factory, Extractor, Mapper>`
  routes ITCH messages across pinned worker threads; a `SynchronousDispatch`
  policy collapses everything to a single-threaded call for profiling and testing.

Note: this is an personal exploration of trading systems and is not tested for non-simulated environments (yet).

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

## Dependencies

All dependencies are fetched either locally or with `FetchContent`.

| Dependency                                                           | Version    | Purpose                                               |
| -------------------------------------------------------------------- | ---------- | ----------------------------------------------------- |
| [abseil-cpp](https://github.com/abseil/abseil-cpp)                   | 20240116.2 | `btree_map`, `uint128` price/qty types                |
| [hwloc](https://github.com/open-mpi/hwloc)                           | 2.12.0     | CPU topology queries and thread pinning               |
| [readerwriterqueue](https://github.com/cameron314/readerwriterqueue) | 1.0.7      | Lock-free SPSC queue for async dispatch               |
| [zlib](https://github.com/madler/zlib)                               | 1.3.1      | Gzip decompression of `.itch.gz` feed files           |
| [GTest](https://github.com/google/googletest)                        | latest     | Unit test framework                                   |
| [BppTree](deps/BppTree)                                              | local      | Augmented B-tree used by `PolicyTree` storage backend |
| [tscns](deps/tscns)                                                  | local      | TSC-based nanosecond clock for benchmarking           |

## Benchmarks

The [benchmarks](benchmarks/) folder contains latency tests for the order book library. These benchmarks measure userspace end-to-end processing time of ITCH messages, from the dispatcher parsing the message to the corresponding book event being delivered to a strategy. Both single-threaded synchronous and multi-threaded queued dispatch modes are evaluated.

### Conditions

I ran these benchmarks on my Framework laptop with a Ryzen 7 7840U on Fedora 42.

### Latency

| Mode                  | P50 (ns) | P95 (ns) | P99 (ns) |
| --------------------- | -------- | -------- | -------- |
| Synchronous           | 120      | 421.0    | 681      |
| Multi-threaded queued | 220      | 1082     | 1899     |

## Usage

### Implementing a Strategy

A _strategy_ receives book events and can query the book. Implement the
`alpbook::strategy::Strategy` concept:

```cpp
import alpbook;

struct MyStrategy {
    using BookType = alpbook::nasdaq::Book<alpbook::nasdaq::PolicyHash, MyStrategy>;

    // Called once per symbol before any events arrive.
    void setBook(BookType* book) { book_ = book; }
    void setAsset(uint16_t assetId) { assetId_ = assetId; }

    // Market events — called by the book on every relevant update.
    void onTopBidChange(alpbook::price_t price, alpbook::quantity_t qty) {}
    void onTopAskChange(alpbook::price_t price, alpbook::quantity_t qty) {}
    void onTrade(alpbook::price_t price, alpbook::quantity_t qty, alpbook::Side side) {}

    // Feed-level lifecycle events.
    void onSystemHalt()   {}
    void onRecoveryStart(){}
    void onSystemRestart(){}

    // Optional: query the book directly for deeper levels or queue position.
    void computeQueuePosition() {
        if (!book_) return;
        auto level = book_->getBidLevel(3);
        auto volumeAhead = book_->getBuyVolumeAhead(myLimitPrice_);
    }

private:
    BookType* book_ = nullptr;
    uint16_t assetId_ = 0;
    alpbook::price_t myLimitPrice_ {};
};

static_assert(alpbook::strategy::Strategy<
    MyStrategy,
    alpbook::nasdaq::Book<alpbook::nasdaq::PolicyHash, MyStrategy>>);
```

Provide a _strategy factory_ to create strategies, and a _sink factory_ so the dispatcher can mint one sink (and its strategies) per worker thread:

```cpp
struct MyStrategyFactory {
    using StrategyType = MyStrategy;
    MyStrategy create(uint16_t assetId) const { return MyStrategy{}; }
};

struct MySinkFactory {
    MyStrategyFactory strategyFactory_;

    using SinkType = alpbook::nasdaq::Sink<
        alpbook::nasdaq::PolicyHash,
        MyStrategy,
        MyStrategyFactory,
        /*timestamped=*/true>;

    explicit MySinkFactory(MyStrategyFactory sf) : strategyFactory_(sf) {}

    template<typename Mapper>
    SinkType create(uint32_t core, Mapper const& mapper) {
        return SinkType(core, mapper, strategyFactory_);
    }
};
```

### Wiring up the Dispatcher

The `Dispatcher` reads ITCH messages off the wire and routes them to per-symbol
sinks running on pinned CPU cores. The latency benchmark uses both the
synchronous (single-threaded) and asynchronous (multi-threaded) specialisations:

```cpp
import alpbook;
import alpbook.itch;
import alpbook.sink.nasdaq;
import alpbook.dispatch;

alpbook::itch::ArrayMapper<> mapper{};
for (uint16_t i = 1; i <= 100; i++)
    mapper.assign(i);

using Slot          = alpbook::itch::ItchSlot</*timestamped=*/true>;
using AsyncDispatcher = alpbook::Dispatcher<Slot,
                            MySinkFactory,
                            alpbook::itch::ItchExtractor,
                            decltype(mapper)>;
using SyncDispatcher  = alpbook::Dispatcher<Slot,
                            MySinkFactory,
                            alpbook::itch::ItchExtractor,
                            decltype(mapper),
                            alpbook::SynchronousDispatch>;  // no threads

MyStrategyFactory strategyFactory{};
MySinkFactory sinkFactory{strategyFactory};
AsyncDispatcher dispatcher{mapper, sinkFactory};

auto result = dispatcher.init(/*maxWorkerThreads=*/4);
if (!result) { /* handle pinning error */ }

// Feed it a gzip-compressed ITCH file.
struct Handler {
    AsyncDispatcher& d;
    void handle(Slot& slot) { d.dispatch(slot); }
};
Handler h{dispatcher};
alpbook::itch::readGzippedItch<Handler, true>("01302020.NASDAQ_ITCH50.gz", h);
```

Sample ITCH data can be downloaded from NASDAQ's FTP server.

## License

MIT License - Copyright (c) 2026 Ben Aepli

See [LICENSE](LICENSE) for full details.

---

**Note**: alpbook requires C++23 modules support. Ensure your compiler and build system are properly configured.
See [docs/building.md](docs/building.md) for detailed setup instructions.
