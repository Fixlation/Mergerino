# Beta release checklist

Use this checklist when cutting a beta or nightly-style Mergerino release.

## Before the release PR

- [ ] Confirm the beta version in `src/common/Version.hpp`
- [ ] Confirm the rounded project version in `CMakeLists.txt`
- [ ] Update the Windows installer metadata if the packaged version changes
- [ ] Move the current `CHANGELOG.md` entries under the beta heading

## After merge

- [ ] Tag the beta release locally and push the tag
- [ ] Build the release artifacts you intend to publish
- [ ] Verify the portable bundle layout before zipping it
- [ ] Produce checksums for the published artifacts
