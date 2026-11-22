-- # Session startup
-- # Desktop daemons (terminus-bar, terminus-launcher, hypridle, mako, ...) are systemd user units.
-- # This file starts none of them, except in the non-UWSM fallback below.
hl.on("hyprland.start", function()
  hl.exec_cmd(o.launch("fcitx5 --disable notificationitem"))
  hl.exec_cmd("terminus compat first-run")
  hl.exec_cmd("terminus compat powerprofiles-init")
  hl.exec_cmd(o.launch("terminus compat hyprland-monitor-watch"))
  hl.exec_cmd("systemctl --user import-environment $(env | cut -d'=' -f 1)")
  hl.exec_cmd("dbus-update-activation-environment --systemd --all")
  hl.exec_cmd("sleep 2 && terminus compat hook post-boot")
  hl.exec_cmd("sleep 4 && terminus-motionfx resume")

  -- # UWSM finalizes before graphical-session.target; direct sessions import explicitly.
  hl.exec_cmd([=[
    if [ -n "${UWSM_FINALIZE_VARNAMES:-}" ]; then
      exec /usr/bin/uwsm finalize
    fi
    systemctl --user import-environment \
      WAYLAND_DISPLAY DISPLAY HYPRLAND_INSTANCE_SIGNATURE \
      XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE \
      PATH TERMINUS_PATH TERMINUS_DESKTOP_PATH &&
    systemctl --user start \
      terminus-bar.service terminus-launcher.service hypridle.service hyprpolkitagent.service swayosd-server.service
  ]=])

  -- # omp starts with the session only on hosts that carry the marker config/hosts/<hostname>/autostart-omp.
  local hostname_file = io.open("/etc/hostname")
  local hn = hostname_file and hostname_file:read("l")
  if hostname_file then hostname_file:close() end
  if hn and o.file_exists(os.getenv("HOME") .. "/.config/terminus-est/hosts/" .. hn .. "/autostart-omp") then
    hl.exec_cmd(o.launch("ghostty --class=com.mitchellh.ghostty.omp -e omp"))
  end
end)
