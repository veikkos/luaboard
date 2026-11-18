# Contributing

Thanks for wanting to help. Bug reports and fixes are welcome, and so are
reports from a real CoreS3
([docs/BRINGUP.md](docs/BRINGUP.md) lists what to check). Apps go to the
[marketplace](https://github.com/veikkos/luaboard-marketplace), which this
repository checks out at `marketplace/`.

## Set up

Clone with `--recursive` (or run `git submodule update --init`), build the
simulator and the tools as in the
[README's quick start](README.md#quick-start), then turn on the pre-push
hooks (pushing the marketplace from `marketplace/` runs `check` too):

```sh
cmake --preset default
cmake --build --preset default --target hooks
```

Day to day:

```sh
cmake --build --preset default --target run      # the simulator window
cmake --build --preset default --target studio   # the studio, which finds the simulator by itself
```

`run` builds in `build/run`, apart from the tests, so both can go at once.

## Tests

```sh
cmake --build --preset default --target check                       # everything CI checks
E2E_ONLY="sound menu" cmake --build --preset default --target e2e   # some end-to-end sections
cmake --build --preset default --target fw                          # the firmware (ESP-IDF, see app/idf/README.md)
```

`check` is lint (rustfmt, clippy), the simulator with warnings as errors,
the unit tests and the end-to-end suite. With the hook on, `git push` runs it
first, and builds the firmware too when the push changes it.

The end-to-end tests drive the simulator through the CLI, against fake
servers in the same process ([pc-tool/tests/e2e](pc-tool/tests/e2e/README.md)).
New behavior gets a check there. Tests never depend on the real date or
time. The simulator runs on a fixed date, and tests move its clock ahead
instead of sleeping.

## Changing an app in marketplace/

`marketplace/` is the marketplace's own checkout, pinned to one of its commits.
To change an app there:

```sh
cd marketplace
git switch main && git pull --rebase    # the pin is a detached commit
# edit, bump "version" in the app's app.json, commit
git push
# wait for the marketplace's Index action to finish, then
git pull --rebase && cd .. && git add marketplace && git commit -m "Marketplace: <what changed>"
```

After every push to the marketplace's `main`, its Index action builds
`index.json` and commits it as "Index for <commit>". That commit is what
boards read, so pin it: wait for the action to finish (on GitHub, or
`gh run watch`), pull it, and only then commit the pin. If the index didn't
change, the action commits nothing and the pushed commit is the one to pin.
Never pin a commit that isn't on the marketplace's GitHub. A push made
before pulling the last index commit is refused: pull and push again.

## How the code is laid out

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the picture. The parts
that matter most when changing something:

- The firmware (`app/src`) uses the ESP-IDF API directly, and the simulator
  provides the same headers (`platform/esp32-mock`). There is no `#ifdef
  SIM` in the firmware: what runs in the simulator is what ships. Chips are
  simulated at the bus level (`platform/sim/models`), so a driver change
  shows up there as it would on the board.
- The firmware stays generic. Something for one device or service (a bike
  computer, a smart plug, an API) belongs in an app, written in Lua. The
  simulator's Bluetooth devices are JSON files an app can carry.
- Board details go in `platform/boards/<board>/board.h`, marked `VERIFY`
  where they come from documentation rather than a board.

## The Lua API

Apps written today must keep working, so the Lua API changes carefully
([docs/LUA_API.md](docs/LUA_API.md#api-version)):

- Every change to it, additions included, raises `APP_LUA_API` in
  `app/src/app_config.h`, and LUA_API.md says what came in which version.
  Until the first release it stays at 1, since no firmware without the
  change is out there.
- A change that breaks existing apps also raises `APP_LUA_API_MIN`, and is
  listed in LUA_API.md. Avoid one if there's any other way.
- A wrong call raises an error. What goes wrong outside the script comes
  back as `nil, err` (LUA_API.md, "Errors"). Names of more than one word
  use an underscore (`text_width`).

## Writing

Docs and comments are plain prose: short sentences, no bold, no em dashes
and no semicolons. The README stays short. Details go in their own files and
get a link, and each fact lives in one place.

## Releasing

Pushing a `v*` tag builds the firmware and the PC tools and publishes them as
a GitHub release ([release.yml](.github/workflows/release.yml)). To cut one:

1. In CHANGELOG.md, finish the section `## X.Y.Z` (no "not released yet"). Its
   first paragraph becomes the note the device shows on its update screen, in
   two lines: keep that paragraph under about 90 characters and make it say
   what changed. A longer one is cut at a word near 120 bytes and ends in "..".
   The whole section becomes the release's notes on GitHub (relative links
   made to point at the tag).
2. Commit it, then tag that commit `vX.Y.Z` and push the tag.

The workflow stops, publishing nothing, if the tag differs from what the
firmware calls itself (`git describe`), or if CHANGELOG.md has no finished
section for the version. A tag with a `-` in it, such as `v1.1.0-rc1`, is a
prerelease: devices are never offered it. It needs its own CHANGELOG
section, `## 1.1.0-rc1`, since the section is found by the tag's exact
version. Every other tag is offered to every device, even one that only
changes the PC tools. By hand, the workflow is a dry run for a tag name you
type: it builds and keeps the files, publishes nothing, and only warns where
a tag would stop (a section still "not released yet", a build that isn't the
tag). The manifest format is in
[docs/PROTOCOL.md](docs/PROTOCOL.md#release-manifest).

## Pull requests

Keep a pull request to one change, with the tests for it, and say what you
checked: the simulator, a real board (which model), Windows or Linux. By
contributing you agree that your work is under the project's
[MIT license](LICENSE).

Security problems: see [SECURITY.md](SECURITY.md) instead of opening an
issue.
