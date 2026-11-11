# Required Android ARM64 libraries

These 12 libraries are included with the native wrapper source on `lite`.
The build validates their SHA-256 checksums against `manifest.json` and copies
them into the build output and app bundle. No other branch or Git history is
needed to build.

The manifest records the original commit, source path, and Git blob identifiers
for provenance only. Asset preparation does not fetch or read Git objects.
Only these required libraries are retained from the former Android filesystem.

Android zlib 1.2.8, libxml2 2.9.9, and ICU 57.1 have been removed. Their original
dependency names map to macOS zlib, libxml2, and ICU through reviewed C interfaces.
The manifest's `host_dependencies` records those mappings. Keep macOS security
updates installed; the system libraries can retain older version strings after
Apple backports a fix. `--verify` tests the actual gzip-header and XML-dictionary
CVE conditions rather than trusting those strings.
