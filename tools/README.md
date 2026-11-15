# Build helpers

Scripts behind some of the build targets (see the top of
[CMakeLists.txt](../CMakeLists.txt)). The tests are in
[pc-tool/tests/e2e](../pc-tool/tests/e2e/README.md).

| File | Purpose |
|---|---|
| `watch_build.cmake`, `run.cmake` | The `run` target: the simulator, and next to it a rebuild whenever a source changes; the simulator boots each new build |
| `install_apps.cmake` | The `apps` target: install your apps (`tools/own_apps.txt`) from `marketplace/` |
| `need_marketplace.cmake` | Stops `apps` and `e2e` with one line when `marketplace/` isn't checked out |
| `release.cmake` | The release workflow's `manifest.json` and `notes.md` from a tag, the built image and CHANGELOG.md; fails if the tag is malformed, the section is missing or says "not released yet", or the image is missing. The `unit` target runs it on `app/test/CHANGELOG.fixture.md` and parses the result with the firmware's parser |
| `studio.cmake`, `flash.cmake` | The `studio` and `flash` targets, with their options read from the environment |
