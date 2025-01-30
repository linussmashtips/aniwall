# Aniwall - Animated Wallpaper Manager for X11

Aniwall is a lightweight animated wallpaper manager for X11 desktop environments. It supports multiple monitors, video stretching across displays, and automatic startup.

## Features

- Play video files as wallpapers
- Support for multiple monitors
- Option to stretch across all monitors
- Automatic startup on login
- Low CPU/GPU usage
- Support for most video formats

## Requirements

- X11-based Linux distribution
- FFmpeg libraries
- Cairo graphics library
- X11 development libraries

## Installation

### From Source

```bash
git clone https://github.com/yourusername/aniwall.git
cd aniwall
sudo ./install.sh
```

### Dependencies

On Ubuntu/Debian:
```bash
sudo apt-get install libx11-dev libcairo2-dev libavcodec-dev \
    libavformat-dev libswscale-dev libxcomposite-dev libxrandr-dev \
    build-essential
```

## Usage

```bash
# Set a video as wallpaper
aniwall set <video_file>

# Set video stretched across all monitors
aniwall set <video_file> --stretch

# Stop the wallpaper
aniwall stop

# Check wallpaper status
aniwall status

# Enable autostart
aniwall enable

# Disable autostart
aniwall disable
```

## Configuration

Configuration files are stored in:
- `~/.local/share/aniwall/` - Video and runtime files
- `~/.config/aniwall/` - User configuration

## Options

- `--stretch`: Stretch video across all monitors
- `--loop`: Loop video playback (default)
- `--no-loop`: Play video once

## Contributing

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- FFmpeg for video decoding
- Cairo for graphics rendering
- X11/Xorg team # aniwall
