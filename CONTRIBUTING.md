# Contributing to esp32-audio-player

Bug reports, hardware test results and pull requests are welcome. The library has not been tested on hardware yet, so a report that says "works on this board with these headphones/DAC" is as useful as a fix.

## Reporting a bug

Open an issue with the bug template and include the ESP-IDF or Arduino core version, the board, the DAC or Bluetooth headphones model, board and PSRAM size, and the log from the serial monitor at the moment of the problem.

## Pull requests

1. Keep the change focused: one fix or one feature per pull request.
2. Build and run the host tests:

   ```bash
   cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test --output-on-failure
   ```

   On Windows without a C compiler, see `cmake/zig-toolchain.cmake`.
3. Build at least one example for the ESP32 (`idf.py build` in `examples/idf/...`) when the change touches code under `src/` that runs on the device.
4. Add a line to the "Unreleased" section of `CHANGELOG.md`.

## Code style

- C17, 4 spaces, 120 columns, `// SPDX-License-Identifier: Apache-2.0` as the first line of every source file.
- English comments that say why the code does what it does.
- The build must stay free of warnings with `-Wall -Wextra`.
- Changes to the decoders, the DSP or the player engine come with a host test.
- Only permissively licensed third-party code (MIT, BSD, Apache-2.0, Unlicense, CC0, zlib). No GPL, LGPL or other copyleft code, including code copied from forums or other projects.

## License

By contributing you agree that your contribution is licensed under the Apache License 2.0, the license of this project.
