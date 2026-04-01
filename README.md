# alpbook

A NASDAQ ITCH 5.0 order book application in C++23.

## Status

Under active reorganization. Previously a generic multi-threaded library; being reshaped into a single-threaded, NASDAQ-specialized binary.

## Building

### Requirements

- C++23 compiler (Clang 16+, GCC 14+)
- CMake 3.30+
- Ninja

```bash
cmake -B build -S . -G Ninja
cmake --build build
```

## TODO

- [ ] TOML configuration (stocks, core pinning, strategy)
- [ ] Stock directory parsing ('R' messages) for ticker → locate mapping
- [ ] Single-threaded state machine (Startup → Live → Recovery → Termination)
- [ ] System event handling ('S' messages)
- [ ] Trading status tracking ('H' messages)
- [ ] Updated benchmarks
- [ ] Documentation

## License

MIT License - Copyright (c) 2026 Ben Aepli
