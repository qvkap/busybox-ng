# BusyBox-ng

BusyBox-ng is a modern, minimalist, and highly optimized fork of the original [BusyBox](https://busybox.net/) project. 

Hello everyone, this is my mini project! I removed utilities that I personally didn't need and added some utilities that I lacked. This fork is heavily stripped down to contain only the most essential tools, making it perfect for lightweight embedded systems, containers, and minimal Linux distributions.

## Features & Optimizations

* **Extreme Size Optimization:** Built statically with aggressive compiler flags (`-Os`, `-ffunction-sections`, `--gc-sections`). The entire statically linked binary weighs around **2.3 MB**.
* **Custom `curl` Applet:** Added a lightweight, built-in `curl` implementation with support for HTTP/HTTPS, custom methods (`-X`), headers (`-H`), and request bodies (`-d`).
* **Modern `wget2`:** Re-written `wget` implementation that acts as a robust `wget2` with native TLS support through the internal `ssl_client`.
* **Zero Dependencies:** Fully static compilation (`CONFIG_STATIC=y`), meaning it requires no external libraries (like glibc or musl) to run on a target system.
* **Debloated:** Removed unused and heavy applets (like `dpkg`, `rpm`, `zstdcat`, `lzop`, etc.) to keep the binary as small as possible while retaining core Unix functionalities.

## Usage

Since this is a multi-call binary, you can use it just like the original BusyBox:

```bash
./busybox curl -s https://example.com
./busybox wget -q -O- https://example.com
```

Or install the symlinks to use the commands natively:

```bash
./busybox --install -s /bin
```

## Authorship

Fork created and maintained by **qvkap**.

*Licensed under GPLv2. See the source distribution for detailed copyright notices.*
