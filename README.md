<div align="center">
  <h1>BusyBox-ng</h1>
  <p><b>A modern, minimalist, and highly optimized fork of the original BusyBox project.</b></p>

  [![License](https://img.shields.io/badge/License-GPLv2-blue.svg)](https://opensource.org/licenses/GPL-2.0)
  [![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)]()
  [![Built with C](https://img.shields.io/badge/Language-C-orange.svg)]()
</div>

---

> *"Hello everyone, this is my mini project! I removed utilities that I personally didn't need and added some utilities that I lacked. This fork is heavily stripped down to contain only the most essential tools, making it perfect for lightweight embedded systems, containers, and minimal Linux distributions."* — **qvkap**

## New Features & Enhancements

We've brought BusyBox into the modern era while keeping it dependency-free and lightning fast:

* **Native HTTP/2 Support**: Complete, dependency-free HTTP/2 protocol implementation (ALPN, HPACK, Flow Control). `curl` and `wget` now natively support multiplexed HTTP/2 over HTTPS!
* **Zstandard (zstd) Integration**: Added streaming `unzstd` decompression, seamlessly integrating `.ko.zst` kernel module loading and `zstdcat`.
* **Modutils Refactored**: Removed ancient 2.4 kernel cruft, modernized `insmod`, `modprobe`, and `depmod` for modern Linux setups.
* **Extreme Size Optimization**: Built statically with aggressive compiler flags (`-Os`, `-ffunction-sections`, `--gc-sections`). The entire statically linked binary weighs around **2.3 MB**.
* **Zero Dependencies**: Fully static compilation (`CONFIG_STATIC=y`), requiring no external libraries (like glibc or musl) to run on a target system.
* **100% GNU Coreutils Compatibility**: All `coreutils` applets have been maximized with full POSIX and GNU feature-flags enabled.
* **Enhanced `vim` Applet**: The builtin `vi` editor is aliased to `vim` and configured with maximum features (undo, regex search, yank marks, colon commands).

## Usage

Since this is a multi-call binary, you can use it just like the original BusyBox:

```bash
# Blazing fast HTTP/2 requests right out of the box
./busybox curl -s https://example.com
./busybox wget -q -O- https://example.com

# Extract modern zstd archives
./busybox unzstd archive.tar.zst
```

Or install the symlinks to use the commands natively on your system:

```bash
./busybox --install -s /bin
```

## Authorship

Fork created and passionately maintained by **qvkap**.

*Licensed under GPLv2. See the source distribution for detailed copyright notices.*
