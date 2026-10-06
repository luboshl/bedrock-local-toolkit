# Repository Guidelines

This is a local add-on for Minecraft Bedrock on Windows: a C# launcher and a custom C++ game module. See [README.md](README.md) for a user overview, [SPECIFICATION.md](SPECIFICATION.md) for requirements and security boundaries, and [FINDINGS.md](FINDINGS.md) for persistent technical findings.

## Requirements and Security

- [SPECIFICATION.md](SPECIFICATION.md) is the canonical source for supported-build, behavior, and security requirements. Before changing game behavior or a security mechanism, read and follow the relevant sections. If anything is unclear, stop and ask for clarification; do not duplicate or relax requirements without authorization.

## Technical Changes

- Keep all human-readable text in the repository in English, regardless of the language used to communicate with the user. This includes documentation, code comments, notes, tests, and user-facing strings such as logs and errors. Preserve identifiers and other literals whose exact wording is required by an API, file format, or game.
- Write all GitHub issues and pull requests in English, regardless of the language used to communicate with the user.
- Before editing, inspect the affected code and relevant sections of the specification and technical findings. Preserve the principles in [FINDINGS.md](FINDINGS.md), especially the distinction between cumulative GameInput state and actual input deltas.
- When porting to a new build, use [FINDINGS.md](FINDINGS.md) only as a guide; revalidate the profiles in `GameMod\*Profile.h` against the specification requirements.
- Distinguish builds, isolated automated tests, and actual in-game verification. Mark behavior as verified only after performing the corresponding check.
- When updating documentation, remove obsolete or one-off notes; `FINDINGS.md` is not a chronological work log.

For code changes, run the relevant checks; use `.\build.ps1 -Test` for a full build and native tests.
