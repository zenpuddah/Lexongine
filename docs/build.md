# Building Lexongine v1

## Supported configuration

The native CLI, plugin loader and filesystem store are verified on macOS arm64.
Use a C++20-capable Apple toolchain, CMake 3.25+ and Python 3. The verification
scripts use only the Python standard library.

The XML frontend requires TinyXML2 **11.0.0**. Supply either a source checkout or
an installed CMake package. The build performs no automatic dependency downloads.

```sh
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEXONGINE_TINYXML2_SOURCE_DIR=/absolute/path/to/tinyxml2-11.0.0
```

For an installed package, replace the source option with
`-Dtinyxml2_DIR=/absolute/path/to/tinyxml2/cmake`. The package version must match.

```sh
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure
```

Executables are placed in `build/debug/bin`. Keep the build directory and generated
`.lexongine-build` sidecars intact: native startup validates executable and dependency
identity. Moving only the executable is not a supported installation method.

## Sanitizers

```sh
cmake -S . -B build/sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEXONGINE_SANITIZERS=ON \
  -DLEXONGINE_TINYXML2_SOURCE_DIR=/absolute/path/to/tinyxml2-11.0.0
cmake --build build/sanitized -j 4
ctest --test-dir build/sanitized --output-on-failure
```

This instruments the project and a supplied TinyXML2 source build with ASan/UBSan.
An installed prebuilt TinyXML2 package and other prebuilt libraries are not thereby
instrumented. Sanitized native integrity tests are substantially slower than normal
tests because they repeatedly validate complete binaries and artifacts.

## Optional Qaedalus reader integration

The engine and reference CLI do not require Arrow, Parquet or the full Qaedalus
source tree. To compile the checked Trade adapter and persisted-acquisition tests,
supply a compatible external checkout and native Arrow/Parquet CMake packages:

```sh
cmake -S . -B build/qaedalus \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLEXONGINE_TINYXML2_SOURCE_DIR=/absolute/path/to/tinyxml2-11.0.0 \
  -DLEXONGINE_QAEDALUS_BRIDGE=ON \
  -DLEXONGINE_QAEDALUS_SOURCE_DIR=/absolute/path/to/qaedalus \
  -DArrow_DIR=/absolute/path/to/Arrow/cmake \
  -DParquet_DIR=/absolute/path/to/Parquet/cmake
cmake --build build/qaedalus -j 4
ctest --test-dir build/qaedalus --output-on-failure
```

The compatible market-data baseline is `zenpuddah/quant-project` commit
`f75cd3c179f3b070f9a85c054ec3704b89f4cb69`. This adapter uses checked reader APIs and
preserves acquisition-scoped ordering; it does not sort events by timestamp or
construct Bars from Trades. Arrow/Parquet 26.0.0 is the verified native package pair.

## CLion

Open the repository root as a CMake project. Set the explicit dependency path in
the CMake profile; use separate build directories for normal and sanitizer profiles.
Local `CMakeUserPresets.json`, IDE state and generated build outputs are ignored.
