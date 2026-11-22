# Style

- Two spaces for indentation, no tabs
- Use bash 5 conditionals: use `[[ ]]` for string/file tests and `(( ))` for numeric tests
- In `[[ ]]`, don't quote variables, but do quote string literals when comparing values (e.g., `[[ $branch == "dev" ]]`)
- Prefer `(( ))` over numeric operators inside `[[ ]]` (e.g., `(( count < 50 ))`, not `[[ $count -lt 50 ]]`)
- For strings/paths with spaces, quote them instead of escaping spaces with `\ ` (e.g., `"$APP_DIR/Disk Usage.desktop"`, not `$APP_DIR/Disk\ Usage.desktop`)
- Shebangs must use `#!/bin/bash` consistently (never `#!/usr/bin/env bash`)
- Scripts under `install/` may be sourced and intentionally omit shebangs

# Command Naming

Desktop helper commands use the `terminus-` prefix. The integration project owns the public `terminus` dispatcher; this directory owns `bin/desktop-core`.

Use `bin/desktop-core --help` for the supported command groups.

# Command Metadata

Commands in `bin/` can declare CLI metadata in comments near the top of the file. `bin/desktop-core` scans the first 80 lines, and tests expect command metadata to remain valid.

Supported metadata keys:

- `# terminus:summary=...` - short help text
- `# terminus:group=...` - command group when it differs from the filename-derived prefix
- `# terminus:name=...` - command name within the group
- `# terminus:args=...` - usage arguments
- `# terminus:examples=...` - examples separated with ` | `
- `# terminus:alias=...` / `# terminus:aliases=...` - alternate routes
- `# terminus:hidden=true` - hide from default command listings
- `# terminus:requires-sudo=true` - mark commands that require sudo

Prefer explicit metadata for user-facing commands. Keep routes consistent with the filename unless there is a deliberate alias or compatibility route.

Example:

```bash
# terminus:summary=Take a screenshot
# terminus:group=capture
# terminus:args=[smart|region|windows|fullscreen] [slurp|copy]
# terminus:examples=terminus screenshot | terminus capture screenshot region
# terminus:aliases=terminus screenshot
```

# Install Scripts

Many scripts under `install/` are sourced via `run_logged` and intentionally do not have shebangs.

Install stage files follow this pattern:

- `install/*/all.sh` lists scripts in execution order
- leaf scripts are sourced by `run_logged "$TERMINUS_INSTALL/path/to/script.sh"`
- avoid `exit` in sourced install scripts unless intentionally aborting the install
- use `$TERMINUS_INSTALL` and `$TERMINUS_DESKTOP_PATH` instead of hard-coded paths; `$TERMINUS_PATH` identifies the integration project
- keep hardware-specific logic under `install/config/hardware/`
- prefer helper commands for package and command checks where available

Raw `command -v`, `pacman`, and `pacman-key` are acceptable in bootstrap/preflight/package-helper contexts where the helper commands may not be available yet or where direct package-manager behavior is the point of the script.

# Helper Commands

Use these instead of raw shell commands:

- `terminus-cmd-missing` / `terminus-cmd-present` - check for commands
- `terminus-pkg-missing` / `terminus-pkg-present` - check for packages
- `terminus-pkg-add` - install packages (handles both pacman and AUR)
- `terminus-hw-external-monitors` - detect a connected external monitor (and similar `hw-*` commands)

Exceptions are allowed for bootstrap, preflight, and package-helper scripts where the helper may not be available yet, where the helper itself is being implemented, or where direct package-manager behavior is required.

# Config Structure

- `config/` - default configs the installer copies to `~/.config/` on a fresh install; the live configuration of this machine is `~/Projects/Terminus-Est/config`, linked as `~/.config/terminus-est`
- `default/themed/*.tpl` - templates with `{{ variable }}` placeholders for theme colors
- `themes/*/colors.toml` - theme color definitions (accent, background, foreground, color0-15)

# Visual Changes

When making visual changes, such as bar styles or desktop appearance, always take and analyze a screenshot after applying the change to verify the result. Use `terminus capture screenshot fullscreen save` for fullscreen screenshots.

