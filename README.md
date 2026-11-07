# mausberry-switch-libgpiod

A GPIO monitoring daemon for [Raspberry Pi][rpi] devices that uses [libgpiod][libgpiod] to watch for signals from a [Mausberry Circuits switch][mausberry-circuits] and safely power off the system.

This is a fork of [t-richards/mausberry-switch][upstream] that replaces the legacy sysfs GPIO interface with the modern libgpiod library.

## What changed from the original

- **libgpiod instead of sysfs**: Uses the modern Linux GPIO character device API via libgpiod instead of the deprecated `/sys/class/gpio` interface.
- **No more glib dependency**: The only runtime dependency is now `libgpiod2`.
- **Proper signal handling**: Uses `sigaction()` and `sig_atomic_t` for safe, portable signal handling.
- **Configuration file support**: Reads pin numbers, shutdown command, and delay from `/etc/mausberry-switch.conf`.
- **Hardened systemd service**: Runs with `ProtectSystem=strict`, `NoNewPrivileges`, and other security restrictions.

## Dependencies

Build dependencies:

- `libgpiod-dev` (>= 1.0)
- `build-essential`
- `dh-autoreconf`

Runtime dependency:

- `libgpiod2` (>= 1.0)

## Building from source

```bash
autoreconf -i -f
./configure
make
sudo make install
```

## Installing

Download the [latest release (v0.9.2)][latest-release] and install it directly on your Pi:

```bash
# Download
wget https://github.com/tomcek42/mausberry-switch-libgpiod/releases/download/v0.9.2/mausberry-switch_0.9.2_armhf.deb

# Install
sudo dpkg -i mausberry-switch_0.9.2_armhf.deb
sudo apt-get -f install
```

## Usage

The `mausberry-switch` systemd service will be automatically enabled and started when you install the package.

```bash
# Stop the service temporarily
sudo systemctl stop mausberry-switch

# Disable the service from automatically starting at boot
sudo systemctl disable mausberry-switch
```

### Configuration

All options are in `/etc/mausberry-switch.conf`:

```ini
[Pins]
Out=23
In=24

[Config]
ShutdownCommand=systemctl poweroff
Delay=0
```

| Option | Default | Description |
|--------|---------|-------------|
| `Out` | 23 | GPIO pin connected to the "out" lead |
| `In` | 24 | GPIO pin connected to the "in" lead |
| `ShutdownCommand` | `systemctl poweroff` | Command executed when the switch is toggled |
| `Delay` | 0 | Seconds to wait before executing the shutdown command |

After changing the configuration, restart the service:

```bash
sudo systemctl restart mausberry-switch
```

## Why not just use the official script?

The [official Mausberry setup script][mausberry-script] polls GPIO state in a bash loop:

```bash
while [ 1 = 1 ]; do
    cat /sys/class/gpio/gpio$GPIOpin1/value
    sleep 1
done
```

This daemon instead uses libgpiod's event-based API to wait for GPIO edge events without burning CPU cycles. The process sleeps until the kernel signals a GPIO state change.

## Why libgpiod over sysfs?

The Linux kernel's sysfs GPIO interface (`/sys/class/gpio`) has been [deprecated since Linux 4.8][gpio-deprecation]. libgpiod is the recommended replacement, providing:

- A stable, well-defined API
- Proper resource management (no manual export/unexport)
- Edge event detection without `poll(2)` on file descriptors
- Better error handling and diagnostics

## License

Available as open source under the terms of the [MIT License][LICENSE].

[gpio-deprecation]: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=79a9bece
[libgpiod]: https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/about/
[LICENSE]: LICENSE
[mausberry-circuits]: http://mausberrycircuits.com/
[mausberry-script]: http://files.mausberrycircuits.com/setup.sh
[rpi]: http://www.raspberrypi.org/
[latest-release]: https://github.com/pauLee/mausberry-switch-libgpiod/releases/latest
[upstream]: https://github.com/t-richards/mausberry-switch
