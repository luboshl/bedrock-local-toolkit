# Repository Guidelines

This is a local add-on for Minecraft Bedrock on Windows: a C# launcher and a custom C++ game module. See [README.md](README.md) for a user overview, [SPECIFICATION.md](SPECIFICATION.md) for requirements and security boundaries, and [FINDINGS.md](FINDINGS.md) for persistent technical findings.

## Requirements and Security

- [SPECIFICATION.md](SPECIFICATION.md) is the canonical source for supported-build, behavior, and security requirements. Before changing game behavior or a security mechanism, read and follow the relevant sections. If anything is unclear, stop and ask for clarification; do not duplicate or relax requirements without authorization.

## Technical Changes

- Before starting or resuming work on a GitHub issue, check the working tree. If there are unrelated local changes, describe them to the user and ask how to proceed. Wait for the user's decision before switching branches or editing files. Never overwrite or discard local changes.
- When implementing a GitHub issue for the first time, run `git fetch origin` after checking the working tree and resolving how to handle any unrelated local changes. If the fetch fails, report the failure and pause before creating the branch or starting implementation. After a successful fetch, create and switch to a dedicated branch from the updated `origin/main` using `git switch --create <issue-branch> origin/main`.
- When resuming work on an issue or addressing pull request feedback, use the existing branch for that issue.
- Keep all human-readable text in the repository in English, regardless of the language used to communicate with the user. This includes documentation, code comments, notes, tests, and user-facing strings such as logs and errors. Preserve identifiers and other literals whose exact wording is required by an API, file format, or game.
- Write all GitHub issues and pull requests in English, regardless of the language used to communicate with the user.
- Before editing, inspect the affected code and relevant sections of the specification and technical findings. Preserve the principles in [FINDINGS.md](FINDINGS.md), especially the distinction between cumulative GameInput state and actual input deltas.
- When porting to a new build, use [FINDINGS.md](FINDINGS.md) only as a guide; revalidate the profiles in `GameMod\*Profile.h` against the specification requirements.
- Distinguish builds, isolated automated tests, and actual in-game verification. Mark behavior as verified only after performing the corresponding check.
- When updating documentation, remove obsolete or one-off notes; `FINDINGS.md` is not a chronological work log.

For code changes, run the relevant checks; use `.\build.ps1 -Test` for a full build and native tests.
