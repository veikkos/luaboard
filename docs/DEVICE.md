# Using the device

## The menu and the buttons

The home screen lists the installed apps, then "+ Marketplace" (see
[Apps from the marketplace](#apps-from-the-marketplace)). Below a divider
stay App updates and Update available when there are any, and Settings.

The whole menu works by touch: tap a row to open or change it, hold an app
for its options, drag a list to scroll it, and tap or drag the brightness
bar (with the buttons: B, then A and C, then B). The buttons do the same:

| Button | In the menu |
|---|---|
| A (left), C (right) | up, down (hold to keep going) |
| B (middle) | open or choose (hold for app options or back) |
| power key | home (hold 1.5 s to sleep, 6 s to turn off) |

The touch buttons are the strip below the SE's screen, with printed icons.
CoreS3 and Lite have no buttons: their menu works by touch alone and shows
no hints for them. In a running app, presses of A, B and C go to the app as events
(`touch.on`) with the menu's timing. Holding B or a short press of the power
key goes home, and that hold never reaches the app.

## Settings and app options

App options cover autostart, clearing data and uninstalling. Settings has
brightness and volume (bars to tap or drag), sleep after (1 to 60 minutes
without a touch, default never), turn off after (the same, on battery,
default never), sleep now, autostart app, check for updates (on by
default), check now (looks for firmware and app updates right away and says
what it found) and time sync.

## The microSD card

Apps can keep files on a microSD card (`sd` in the Lua API). The card has
to be FAT formatted, FAT16 or FAT32. One that comes as exFAT (most cards
over 32 GB) needs formatting as FAT32 on a computer first. The device
itself doesn't use the card and never formats one.

## Asleep and off

Asleep, the screen is off and whatever ran keeps running: a timer still
rings, a dashboard still gets its data. On battery the device also saves
power while asleep. It goes to sleep after Sleep after's minutes without a
touch, with Sleep now, or when the power key is held 1.5 s. A touch or the
power key wakes it and does nothing else.

Off, the power is cut: on battery the board's power chip turns it off, and
nothing runs. Turn off after does it once the device hasn't been touched for
that long, asleep or not, unless the app that runs keeps it on
(`sys.keep_on(true)`) or keeps the screen awake. On USB power it stays on. An app can turn the device off too
(`sys.off()`), and holding the power key 6 s does it in the power chip
itself. The power key turns it on again. An app can also ask to be woken:
the device then turns itself on in time.

On USB power the device doesn't power down when an app turns it off: the
app stops and the screen goes off until the power key is pressed (or the
app's wake-up time comes), and a computer connected to it stays connected.
The CoreS3 SE, which has no battery, is always in this case.

When the device turned itself off while an app was running, that app
starts again when the device comes on, once. A finger held on the screen
while it starts skips that, as it skips autostart. On USB power, hold the
finger on the screen while pressing the power key: the menu comes instead
of the app.

## Firmware updates

With Wi-Fi joined and Check for updates on, the device looks for a newer
release on GitHub about a minute after it connects, and then once a day. It
only reads a small file: nothing is installed unless you tap Install. A check
that fails (no network, GitHub unreachable) is silent and tried again in ten
minutes, then in an hour, then daily. Settings > Check now looks right away.

When a newer release is known, an accent-colored row "Update available" shows
in the home menu above Settings. Its screen tells the version. Turning
Check for updates off removes the row and stops the checks. Only newer
releases are offered, never a downgrade or a prerelease.

Tapping the row opens a screen with the version, the size, the release's one
line of notes, and the installed apps the new firmware won't run (apps made
for a Lua API version outside what that firmware supports). Install and Back
are the two buttons.

An installed app the firmware won't run is kept, not removed: the home menu
shows it greyed and marked "won't run", and a tap on it opens its options,
which say why and offer Uninstall. As the autostart app it is skipped at
boot (the setting stays). A firmware that runs it again, or a new version
of the app, brings it back.

Should a release ever come in a form this firmware can't install by itself,
the row says "Manual update needed" instead, and its screen tells where to
look: that release is installed from a computer.

Install downloads the release straight into the other firmware slot, checks
it against the size and SHA-256 in the release's manifest, and shows the same
progress screen as an update pushed from a PC. Then the device restarts into
it, and the usual rollback applies: if the new firmware keeps crashing, the
device goes back to the old one (see [Firmware update](PROTOCOL.md#firmware-update-ota)).
If the download fails (Wi-Fi drops, the hash is wrong, the server answers
with an error), nothing changes: the running firmware stays, and the screen
says what went wrong with a way back to the menu. Apps, settings and Wi-Fi
stay through an update.

## Apps from the marketplace

The [marketplace](https://github.com/veikkos/luaboard-marketplace) is where apps are published, each in its own folder
with a version, a license and a README. With Wi-Fi joined and Check for
updates on, the device reads the marketplace's index along with the firmware
check (a minute after it connects, then daily). Nothing is installed unless
you tap.

"+ Marketplace", the last row of the home list, opens every app this firmware
can run, by name. A tag at the right says "installed", "update" (a newer
version is published), or "(i)" for an app that needs your own keys or a
login. An app's screen tells who made it, its version and license and what
it does, with Install (or Update, or Open). For an app that needs setup it
also shows where the steps are, as an address and as a QR code that opens the
app's README on a phone. Keys go in from a computer: the studio, or
`luaboard secrets set <app> <name> <value>`.

When installed apps have newer versions, their rows say "update" and an
"App updates (N)" row shows above Settings. Its screen lists them with the
installed and the new version. Update all goes through them one by one, or
tap one app. An update fetches only the files that changed, checks each
against the size and SHA-256 the index gives, and swaps the new version in
at once: a failed download, or a power cut half way, leaves the installed
version as it was. The app's secrets and stored values stay. An app that
fails says why in its row, and the others still update.

An app is matched to the marketplace by its folder name, whatever installed
it. A version installed from a git folder (`1.0.0+a1b2c3d`, with `-dirty`
when it had changes) counts as its base version: it is offered `1.0.1`, not
`1.0.0` again.

## Crashes and safe mode

After a crash the device says so on its screen as it starts again, and
`luaboard crash` (or the studio's console) shows where it happened. A tap
goes on at once. A finger on the screen at boot, or three crashes in a row,
skips autostart.
