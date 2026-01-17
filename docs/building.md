# Building

## Dependency Management

This project uses `FetchContent` to manage dependencies. 

## Building

`CMakePresets.json` contains a preset for building with clang. To use it, run:

```bash
cmake --preset included
cmake --build build
```

You can also configure a `CMakeUserPresets.json` file to set additional options and override the toolchain file. For
example, with my debug profile:

```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "local",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build",
      "cacheVariables": {
        "CMAKE_C_COMPILER": "gcc",
        "CMAKE_CXX_COMPILER": "g++",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "CMAKE_BUILD_TYPE": "Debug"
      }
    }
  ]
}
```

Then, we can simply switch to the `local` preset and build.

This project has been tested with Clang 20 and GCC 14. It should build with MSVC, but I have not tested it.

## C++ Modules

This project uses C++23 modules. The alpbook library is implemented as a single module.

### Module Usage

In your code, import the module instead of including headers:

```cpp
import alpbook;  // Import the module

int main() {
    // Use alpbook functionality
}
```

### Build Requirements

- **CMake:** 3.28+ (3.30+ recommended)
- **C++23 compiler:**
  - Clang 16+ (Clang 20 recommended)
  - GCC 14+
  - MSVC 19.34+ (Visual Studio 2022)
- **Build system:** Ninja (recommended for best module support)
