# NativeMusicWrapperKit

The Apple silicon macOS binary Swift package includes a static XCFramework, a precompiled worker,
and the 12 checksum-validated ARM64 SDK libraries. Build a new distribution from the wrapper repository:

```sh
python3 script/package_library.py --output build/NativeMusicWrapperKit
```

Copy the resulting directory to `Vendor/NativeMusicWrapperKit` in the host project and add it as
a local Swift package dependency of the app target. Add one Run Script phase invoking
`/bin/bash "$SRCROOT/Vendor/NativeMusicWrapperKit/embed-runtime.sh"`. Set its input and output
file lists to the package's `inputs.xcfilelist` and `outputs.xcfilelist`. The script copies the assets
and signs the worker with the host's identity and sandbox-inheritance entitlements.

```swift
import NativeMusicWrapperKit

let configuration = NativeRuntimeConfiguration(profile: appPrivateProfileURL)
let wrapper = EmbeddedWrapperBackend(configuration: configuration)
let reply = try await wrapper.send(WrapperRequest(route: "/artist", query: [
    "adamId": "329155759", "storefront": "ph"
]))
```

`reply.metadata` contains the wrapper's JSON envelope. Artist lookup defaults to the US storefront
and uses a public web developer token. The profile includes the normal portrait, biography, origin,
formation or birth date, genres and editorial notes. `artistLogo.url` is a transparent PNG.
`wideArtwork.url` is a 1600 × 900 landscape JPEG. Apple’s dedicated centered background takes
precedence over a hero image that recommends the `vf` crop. Missing images are JSON null.
Both image objects also include a size template, width and height. Reading this metadata does not
start the worker or download an image.
