#!/bin/bash

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root"
    exit 1
fi

# Install dependencies
apt-get update
apt-get install -y \
    libx11-dev \
    libcairo2-dev \
    libavcodec-dev \
    libavformat-dev \
    libswscale-dev \
    libxcomposite-dev \
    libxrandr-dev \
    build-essential

# Create build directory
mkdir -p build

# Create config directory in user's home
sudo -u $SUDO_USER mkdir -p /home/$SUDO_USER/.config/aniwall

# Compile and install the daemon
gcc -o build/aniwall-daemon src/aniwall.c \
    -lX11 -lcairo -lavcodec -lavformat -lavutil -lswscale -lXrandr \
    -O2 -Wall
install -m 755 build/aniwall-daemon /usr/local/bin/

# Set proper permissions
chown root:root /usr/local/bin/aniwall-daemon
chmod u+s /usr/local/bin/aniwall-daemon

# Install the control script
install -m 755 aniwall /usr/local/bin/

# Install systemd service
install -m 644 aniwall.service /etc/systemd/user/

# Enable autostart for current user
DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u $SUDO_USER)/bus" \
XDG_RUNTIME_DIR="/run/user/$(id -u $SUDO_USER)" \
sudo -E -u $SUDO_USER systemctl --user daemon-reload

DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u $SUDO_USER)/bus" \
XDG_RUNTIME_DIR="/run/user/$(id -u $SUDO_USER)" \
sudo -E -u $SUDO_USER systemctl --user enable aniwall

# Create required directories
mkdir -p /home/$SUDO_USER/.local/share/aniwall
chown -R $SUDO_USER:$SUDO_USER /home/$SUDO_USER/.local/share/aniwall
chown -R $SUDO_USER:$SUDO_USER /home/$SUDO_USER/.config/aniwall

# Add user to required groups
usermod -a -G video,render $SUDO_USER

echo "Installation complete!"
echo "Use 'aniwall set <video>' to set a wallpaper"
echo "Aniwall will automatically start on next login" 