DANOS-Open generic DPDK physical lab runner (Debian trixie)

At the boot: prompt, type one of these labels and press Enter:
  live           Start LIVE diagnostics (does not write to disk)
  install        Install via VGA keyboard (selected disk erased after confirmation)
  install-serial Install with ttyS0 as the Linux installer console

The boot prompt waits indefinitely. There is no automatic LIVE timeout.

The installer requires the exact confirmation ERASE /dev/<whole-disk>.
It does not automatically select or overwrite a disk.
This lab image enables root autologin on local consoles; do not expose it to
an untrusted network. Configure management mTLS before enabling mgrd.
