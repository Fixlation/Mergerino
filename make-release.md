# Stable release checklist

Use this checklist when cutting a stable Mergerino release.

## Before the release PR

- [ ] Confirm the version in `src/common/Version.hpp`
- [ ] Confirm the project version in `CMakeLists.txt`
- [ ] Update any Windows installer version metadata before packaging
- [ ] Move the current `CHANGELOG.md` entries under a real version heading

## After merge

- [ ] Tag the release locally and push the tag
- [ ] Build the Windows release payload
- [ ] Verify the portable layout includes `mergerino.exe`, `modes`, and `updater.1`
- [ ] Produce checksums for the release artifacts
- [ ] Upload the final release artifacts to your release destination
