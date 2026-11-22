# Terminus-Est

This directory contains the owned desktop helpers, defaults, themes, and package
maintenance for Severian's Terminus-Est environment, derived from the desktop helper
code originally created by DHH and Basecamp.

The integration project's `terminus` command delegates desktop helper routes to
`bin/desktop-core`. Owned helper commands use the `terminus-*` prefix.
`TERMINUS_DESKTOP_PATH` identifies this directory; `TERMINUS_PATH` identifies the
integration project. Native menus use Terminus-Bar and Terminus-Settings, application
launching uses Terminus-Launcher, and the file manager is Strata.

Package updates do not replace this source tree or replay retired upstream
migrations. External package and theme-extension identifiers retain their real published names.

## License

Terminus-Est is released under the
[MIT License](https://opensource.org/licenses/MIT).
