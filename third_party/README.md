# third_party

## metal-cpp 26.4
C++ bindings for Metal/Foundation/QuartzCore (header-only), Apache 2.0 — see `metal-cpp/LICENSE.txt`.

- Source: https://developer.apple.com/metal/cpp/files/metal-cpp_26.4.zip
- SHA-256 of the zip: `4e7587c0f351d782f1f3e7c67c649b40d0ba780c051b2f6729f00ddc61ada28c`
- Copied as-is, minus `SingleHeader/` (generator script) and a stray editor swap file.
- The implementation is instantiated once, in `src/render/metal_impl.cpp`.
