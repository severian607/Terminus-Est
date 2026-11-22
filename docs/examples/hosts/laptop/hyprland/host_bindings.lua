-- # Laptop bindings that replace the shared ones (loaded after application_bindings)

-- Locking is real on a laptop: Super+L starts the authenticating lock (terminus-lock).
hl.unbind("SUPER + L")
o.bind("SUPER + L", "Secure lock", "terminus-lock force")

-- Super+Shift+L shows the screensaver. It does not lock.
o.bind("SUPER + SHIFT + L", "Screensaver", "terminus-screensaver force")
