# sane-ipp

SANE backend for network scanners that speak IPP Scan (PWG 5100.17).
It discovers scanners on the local network via DNS-SD (Avahi) and scans
from them, so any SANE frontend (scanimage, xsane, simple-scan) can use them.

## Setup

Install the build dependencies (Debian/Ubuntu):

```sh
sudo apt install build-essential pkg-config libsane-dev \
    libavahi-client-dev libcups2-dev libpng-dev
```

Build and install:

```sh
make
sudo make install
```

This installs `libsane-ipp.so.1` into the SANE backend directory, enables
the backend in `/etc/sane.d/dll.d/ipp`, and installs `/etc/sane.d/ipp.conf`.

Check that your scanner is found:

```sh
scanimage -L
```

To use a scanner that discovery can't see, name it directly:

```sh
SANE_IPP_DEVICE="MyScanner:ipp://192.168.1.20:631/ipp/scan" scanimage -L
```

## Debugging

- `SANE_DEBUG_IPP=1..4` prints messages on stderr (1 errors … 4 full IPP messages).
- Set `trace = ~/ipp/trace` in the `[debug]` section of `/etc/sane.d/ipp.conf`
  to get a full log per device in `~/ipp/trace/<program>-<device>.log`.
