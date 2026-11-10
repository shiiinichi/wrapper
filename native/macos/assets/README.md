# Required Android ARM64 libraries

These 17 libraries are included with the native wrapper source on `lite`.
The build validates their SHA-256 checksums against `manifest.json` and copies
them into the build output and app bundle. No other branch or Git history is
needed to build.

The manifest records the original commit, source path, and Git blob identifiers
for provenance only. Asset preparation does not fetch or read Git objects.
Only these required libraries are retained from the former Android filesystem.
