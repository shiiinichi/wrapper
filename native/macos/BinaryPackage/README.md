# NativeMusicWrapperKit

The binary SwiftPM package distributes a C++ static XCFramework, a native worker and 12 verified Android ARM64 libraries. The public header is `NativeMusicWrapperKit.h`. The package manifest declares the binary target.

```sh
./script/package_library.sh --output build/NativeMusicWrapperKit
```

Add the generated Swift package to your app. Add a Run Script build phase invoking `/bin/bash "$SRCROOT/Vendor/NativeMusicWrapperKit/embed-runtime.sh"` with the package’s `inputs.xcfilelist` and `outputs.xcfilelist`. Xcode embeds the worker and assets and signs the worker with its host identity and sandbox-inheritance entitlements.

Create a runtime with `nmw_create`, passing the embedded worker, SDK asset folder and app-private profile. Submit requests with `nmw_send` from a background queue. Each operation has an independent cancellation handle. Dispose returned buffers using `nmw_free_bytes` and release runtime handles after pending calls finish. `nmw_import_profile` stages an explicit profile import; reserve account operations while presenting sign-in. The C++ sign-in API exchanges prompts and answers over private pipes.

`nmw_send` supports `/profile` with query `{}` for the active account's cached
Apple Music profile. Its data contains `profile_present`, nullable `name`, nullable
`handle`, and nullable `image` (`url` and optional `width`/`height`). A missing
profile or photo is a normal optional result; signed-out requests return envelope
code `401`. Image URLs are validated HTTPS Apple CDN metadata and may contain
`{w}`, `{h}`, `{c}` or `{f}` templates. The response supplies photo metadata and
omits internal account identifiers. Query `{"refresh":"true"}` explicitly requests an
SDK refresh, with an 8-second SDK timeout and a 12-second worker wall deadline
after admission. Refresh failure returns the normal backend error; cancellation
and timeout reset the worker. Live-account cached reads, SDK network refresh and
the returned photo URL were verified on 2026-11-18.

Every device request uses the same Android Music client identity. `/m3u8` returns the device-selected playlist or protected file. Request `representation=hls` when a player requires HLS packaging for a direct file. Source quality comes from the selected audio header. Catalog tags do not choose playback assets.
