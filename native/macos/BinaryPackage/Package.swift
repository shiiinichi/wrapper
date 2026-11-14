// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "NativeMusicWrapperKit",
    platforms: [.macOS("15.0")],
    products: [.library(name: "NativeMusicWrapperKit", targets: ["NativeMusicWrapperKit"])],
    targets: [.binaryTarget(name: "NativeMusicWrapperKit", path: "Artifacts/NativeMusicWrapperKit.xcframework")]
)
