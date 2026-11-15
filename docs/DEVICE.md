# Using the device

## The menu and the buttons

The whole menu works by touch: tap a row to open or change it, hold an app
for its options, drag a list to scroll it, and tap or drag the brightness
bar (with the buttons: B, then A and C, then B). The buttons do the same:

| Button | In the menu |
|---|---|
| A (left), C (right) | up, down; hold to keep going |
| B (middle) | open or choose; hold for app options or back |
| power key | home; hold 1.5 s to turn the screen off |

The touch buttons are the strip below the screen, with printed icons on the
SE. In a running app, presses of A, B and C go to the app as events
(`touch.on`) with the menu's timing; holding B or a short press of the power
key goes home, and that hold never reaches the app.

## Settings and app options

App options cover autostart, clearing data and uninstalling. Settings has
brightness and volume (bars to tap or drag), screen off (after 1 to 60
minutes without a touch, default never), turn off after (5 to 60 minutes
asleep, default never), sleep now, autostart, check for updates (on by
default) and time sync.

## The microSD card

Apps can keep files on a microSD card (`sd` in the Lua API). The card has
to be FAT formatted, FAT16 or FAT32; one that comes as exFAT (most cards
over 32 GB) needs formatting as FAT32 on a computer first. The device
itself doesn't use the card and never formats one.

## Asleep and off

Asleep, the screen is off and whatever ran keeps running: a timer still
rings, a dashboard still gets its data. A touch or the power key wakes it
and does nothing else.
On battery the device also saves power while asleep.

Off, the board is powered down. The Turn off after setting does it once
the device has been asleep that long on battery, unless the app that runs
keeps it on (`sys.keep_on(true)`); a device that never sleeps never turns
off. An app can turn the device off too (`sys.off()`), and so does a 6
second press of the power key. The power key turns it on again. An app can
also ask to be woken: the device then turns itself on in time.

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
minutes, then in an hour, then daily.

When a newer release is known, an accent-colored row "Update available" shows
in the home menu above Settings; its screen tells the version. Turning
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

## Crashes and safe mode

After a crash the device says so on its screen as it starts again, and
`luaboard crash` (or the studio's console) shows where it happened. A tap
goes on at once. A finger on the screen at boot, or three crashes in a row,
skips autostart.
