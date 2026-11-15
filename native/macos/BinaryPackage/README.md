# NativeMusicWrapperKit

The binary SwiftPM package distributes a C++ static XCFramework, a native worker and 12 verified Android ARM64 libraries. The public header is `NativeMusicWrapperKit.h`. The package manifest declares the binary target.

```sh
./script/package_library.sh --output build/NativeMusicWrapperKit
```

Add the generated package as a local app dependency. Add a Run Script build phase invoking `/bin/bash "$SRCROOT/Vendor/NativeMusicWrapperKit/embed-runtime.sh"` with the package’s `inputs.xcfilelist` and `outputs.xcfilelist`. Xcode embeds the worker and assets and signs the worker with its host identity and sandbox-inheritance entitlements.

Create a runtime with `nmw_create`, passing the embedded worker, SDK asset folder and app-private profile. Submit requests with `nmw_send` from a background queue. Each operation has an independent cancellation handle. Dispose returned buffers using `nmw_free_bytes` and release runtime handles after pending calls finish. `nmw_import_profile` stages an explicit profile import; reserve account operations while presenting sign-in. The C++ sign-in API exchanges prompts and answers over private pipes.

Every device request uses the same Android Music client identity. `/m3u8` returns the device-selected playlist or protected file. Request `representation=hls` when a player requires HLS packaging for a direct file. Source quality comes from the selected audio header. Catalog tags do not choose playback assets.
