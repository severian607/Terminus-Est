# Copy over Terminus-Est configs
mkdir -p ~/.config
cp -R ~/.local/share/terminus-est/config/* ~/.config/

# Use default bashrc from Terminus-Est
cp ~/.local/share/terminus-est/default/bashrc ~/.bashrc
