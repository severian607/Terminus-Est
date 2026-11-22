update-desktop-database ~/.local/share/applications

# File manager
xdg-mime default org.gnome.Nautilus.desktop inode/directory

# Images with Pinta
xdg-mime default com.github.PintaProject.Pinta.desktop image/png
xdg-mime default com.github.PintaProject.Pinta.desktop image/jpeg
xdg-mime default com.github.PintaProject.Pinta.desktop image/gif
xdg-mime default com.github.PintaProject.Pinta.desktop image/webp
xdg-mime default com.github.PintaProject.Pinta.desktop image/bmp
xdg-mime default com.github.PintaProject.Pinta.desktop image/tiff
xdg-mime default com.github.PintaProject.Pinta.desktop image/svg+xml

# PDFs with Document Viewer
xdg-mime default org.gnome.Evince.desktop application/pdf

# Browser
xdg-settings set default-web-browser firefox.desktop
xdg-mime default firefox.desktop x-scheme-handler/http
xdg-mime default firefox.desktop x-scheme-handler/https
xdg-mime default firefox.desktop x-scheme-handler/about

# Video with VLC
xdg-mime default vlc.desktop video/mp4
xdg-mime default vlc.desktop video/x-msvideo
xdg-mime default vlc.desktop video/x-matroska
xdg-mime default vlc.desktop video/x-flv
xdg-mime default vlc.desktop video/x-ms-wmv
xdg-mime default vlc.desktop video/mpeg
xdg-mime default vlc.desktop video/ogg
xdg-mime default vlc.desktop video/webm
xdg-mime default vlc.desktop video/quicktime
xdg-mime default vlc.desktop video/3gpp
xdg-mime default vlc.desktop video/3gpp2
xdg-mime default vlc.desktop video/x-ms-asf
xdg-mime default vlc.desktop video/x-ogm+ogg
xdg-mime default vlc.desktop video/x-theora+ogg
xdg-mime default vlc.desktop application/ogg

# Audio with VLC
xdg-mime default vlc.desktop audio/mpeg
xdg-mime default vlc.desktop audio/flac
xdg-mime default vlc.desktop audio/ogg
xdg-mime default vlc.desktop audio/wav
xdg-mime default vlc.desktop audio/aac
xdg-mime default vlc.desktop audio/opus
xdg-mime default vlc.desktop audio/webm

# Text files with Zed
xdg-mime default dev.zed.Zed.desktop text/plain
xdg-mime default dev.zed.Zed.desktop text/english
xdg-mime default dev.zed.Zed.desktop text/markdown
xdg-mime default dev.zed.Zed.desktop text/html
xdg-mime default dev.zed.Zed.desktop text/css
xdg-mime default dev.zed.Zed.desktop text/javascript
xdg-mime default dev.zed.Zed.desktop text/x-python
xdg-mime default dev.zed.Zed.desktop text/x-csrc
xdg-mime default dev.zed.Zed.desktop text/x-c++src
xdg-mime default dev.zed.Zed.desktop text/x-shellscript
xdg-mime default dev.zed.Zed.desktop text/x-rust
xdg-mime default dev.zed.Zed.desktop text/x-go
xdg-mime default dev.zed.Zed.desktop text/x-java
xdg-mime default dev.zed.Zed.desktop text/x-makefile
xdg-mime default dev.zed.Zed.desktop text/xml
xdg-mime default dev.zed.Zed.desktop application/json
xdg-mime default dev.zed.Zed.desktop application/xml
xdg-mime default dev.zed.Zed.desktop application/x-yaml
xdg-mime default dev.zed.Zed.desktop application/toml
xdg-mime default dev.zed.Zed.desktop application/x-shellscript
