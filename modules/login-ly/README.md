# login-ly

Ly 1.4.1 for the Terminus-Est login screen (`ly-kmsconvt@tty2`, run by kmscon). Two patches on top of the upstream tag `v1.4.1`:

| Patch | What |
| --- | --- |
| `patches/waitpid-eintr.patch` | Retries `waitpid` on `EINTR` (carried over from the earlier local build, `ly-1.4.1-1.1`). |
| `patches/terminus-constellation.patch` | Constellation password and the `terminus` background animation. |

Package: `ly 1.4.1-1.2`. Build needs a Zig 0.16.x toolchain (Ly 1.4.1 declares `minimum_zig_version 0.16.0`).

## What it does

**`password_style = constellation`** (default `asterisk` = stock Ly, byte for byte)

- Every typed character adds one star to a constellation in the middle of the screen, around the login box. The password field keeps its normal masked dots (`asterisk`), and F7 reveal works as in stock Ly.
- Star positions come from a PRNG seeded once per Ly start from OS entropy and indexed by character position only. Only the *length* of the field is read, so position, glyph (`✦ ✧ ⋆ · *`) and brightness never depend on which key was typed, and nothing derived from the password is logged or drawn. Stars form a chain (each near the previous one), keep a minimum distance, stay in the central part of the screen (outside the outer fifth horizontally and sixth vertically), avoid the login box and, with `animation = terminus`, the boot-log rows. The layout is regenerated on resize. Beyond 96 characters no further stars are added.
- A new star twinkles (bright `✦`, then settles). Backspace/Delete remove stars, Ctrl+U removes all.
- **Failure:** the stars flash in the error colour and fall and fade over 400 ms, then the field clears as stock (if `clear_password` is off and the error is a plain PAM auth error, they only flash and stay).
- **Success:** lines are drawn between the stars in typing order (box-drawing, diagonal and dot glyphs by segment direction, about 500 ms), then the whole constellation blinks three times in the accent colour (`border_fg`, about 600 ms), about 1.25 s in total. Then the session starts exactly as in stock Ly.

**`animation = terminus`**: a night sky in the terminal style. A dim boot-log ticker in the top rows (below row 0, kept for key hints) types out lines (`[  ok  ] reached target graphical interface`, kernel-style timestamps) and scrolls up like a console; the sun, sword and grass field sit below it, with the grass on the last rows of the screen and the sword drawn in front of it so its blade letters stay readable. Sparse dim twinkling stars (`.` `·` `˙`) drift very slowly between the two, with an occasional shooting star. Nothing but the ticker is drawn in its rows, so its text is never covered. Colours are blends of `fg`, `border_fg` and `bg`. Everything is a function of the clock; per-frame cost is bounded by the screen size. Respects `animation_timeout_sec` and `animation_frame_delay` (40 recommended). It is drawn under the box, so the box stays readable.

New config keys:

| Key | Values | Default |
| --- | --- | --- |
| `password_style` | `asterisk`, `constellation` | `asterisk` |
| `animation` | existing values plus `terminus` | `none` |

`config-snippet.ini` has the keys; `res/config.ini` inside the patch documents them.

## The success hook

How Ly is structured: the UI process forks a child that runs `auth.authenticate` (PAM). The UI process is blocked in `waitpid` for the whole session, so it cannot draw anything once the password is accepted. The forked child, however, holds a copy of the whole UI state, including the termbox buffer and the constellation, and shares the TTY.

So the hook lives in the auth child, in `auth.authenticate`, directly **after `pam_acct_mgmt` succeeded** and before `pam_setcred`:

```
pam_authenticate -> pam_acct_mgmt -> [on_accepted()] -> pam_setcred -> pam_open_session -> fork session
```

`on_accepted` is a new optional callback in `AuthOptions` (`null` in asterisk mode and for autologin, so stock flow is untouched). It does not change any PAM call, argument or order, and it handles no credentials. What it does (`onPasswordAccepted` in `main.zig`):

1. Returns at once if there are no stars.
2. Forks a throw-away grandchild. The grandchild redraws all layers (background animation, constellation, box) every 30 ms for 1.25 s and calls `_exit(0)`; it never touches PAM or the password.
3. The auth child waits for it with `waitpid(WNOHANG)` polling and a **hard 1.5 s deadline**; on timeout it sends `SIGKILL` and reaps it. Fork failure or any error means no animation, login continues.

Safety argument:

- **Auth order unchanged:** the only addition is one call between two existing PAM calls, executed only after account validation succeeded.
- **Bounded:** the animation ends by itself after 1.25 s, the wait is capped at 1.5 s, so a login is delayed by 1.5 s at most. A crash or hang in the animation kills only the grandchild, not the login (this is why it is a separate process).
- **No password-derived output:** the animation uses only the star count taken from the field length and the PRNG layout. The password buffer is never read by the new code except `text.items.len`.
- **TTY ownership:** the animation completes before `startSession` sends the "release tty" signal, so the UI process still owns the terminal.

The failure animation runs in the UI process after it reclaims the terminal, as a time-based widget state (no blocking).

## Build

```bash
cd modules/login-ly
ZIG=$HOME/.cache/terminus-est/zig-x86_64-linux-0.16.0/zig makepkg -f
```

`ZIG` is optional: without it the PKGBUILD uses `zig` from PATH, which must be 0.16.x. The 0.16.0 toolchain is a plain tarball from <https://ziglang.org/download/> (sha256 `70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00`); unpack it anywhere. Result: `ly-1.4.1-1.2-x86_64.pkg.tar.zst`. Sources and the vendored Zig dependencies are downloaded by `makepkg`; no root needed.

## Test without root

A real PAM login needs root, but the UI can be tried in tmux with a copy of the config (`--config` takes the **directory** holding `config.ini` and `lang/`):

```bash
mkdir -p /tmp/lytest && cp -r /etc/ly/config.ini /etc/ly/lang /tmp/lytest/
# in /tmp/lytest/config.ini: ly_log = /tmp/lytest/ly.log, save = false,
# animation = terminus, password_style = constellation
tmux new-session -d -s lyt -x 200 -y 50 'ly-dm --config /tmp/lytest'   # or the built src/pkg binary
tmux send-keys -t lyt Tab a b c d e      # five stars
tmux capture-pane -t lyt -p -e
```

## Install

```bash
./install.sh
```

It backs up `/etc/ly/config.ini`, runs `sudo pacman -U` on the package, and sets `password_style = constellation` and `animation = terminus` (idempotent). Nothing is restarted: it takes effect the next time `ly-kmsconvt@tty2` starts (next boot or logout).

## Roll back

```bash
sudo pacman -U ~/'Projects/Sakura Mochi Login/ly-eintr-fix/ly-1.4.1-1.1-x86_64.pkg.tar.zst'   # or LY_ROLLBACK_PKG
sudo cp -a /etc/ly/config.ini.bak-<timestamp> /etc/ly/config.ini   # printed by install.sh
```

Setting `password_style = asterisk` and `animation = dur_file` (or `none`) in `/etc/ly/config.ini` is enough to get the old look back without a rollback of the package.

## Broken login screen

1. Ctrl+Alt+F3 for a plain console, log in.
2. Run the two rollback commands above (or edit `/etc/ly/config.ini`: `password_style = asterisk`, `animation = none`).
3. `sudo systemctl restart ly-kmsconvt@tty2` or reboot.
