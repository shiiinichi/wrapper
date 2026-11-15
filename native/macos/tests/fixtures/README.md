# Loader test inputs

`tls-fixture.elf` is the deterministic synthetic ARM64 ELF used by the TLS, CoreFoundation and service ABI probes. Its SHA-256 is `7692c25c5478fcb400f4a5dff25fa1477211f93bea074306f73f063cff7af2d6`. The fixture and its assembly are preserved from the former `native/macos/test_fixtures.py` generator (Git blob `bd622d98da7270ddbae3ae8807de06f6161a2969`).

`elf-cases.json` describes mutations of the public, pinned Blocks runtime and their expected loader outcomes. Each mutation verifies its original bytes before applying a change. The fixtures are test inputs and are excluded from the distributed package.
