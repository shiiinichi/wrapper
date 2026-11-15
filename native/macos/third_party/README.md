# Native runtime headers

- `json/json.hpp` is the unmodified single header from nlohmann/json v3.11.3, commit `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03`. Its MIT license is distributed with the framework and desktop app.
- `http/httplib.h` is based on the cpp-httplib v0.18.2 header used by the original wrapper-lite at `e51e370a6b0660ed9d722fff1add079098aeb935:lite/httplib.h`. A local patch propagates response connection closure to the keep-alive loop, including early rejection before reading a body. Its MIT license is distributed with the standalone desktop app. The embedded framework uses the private worker pipe.
