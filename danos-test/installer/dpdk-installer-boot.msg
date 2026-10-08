DANOS-Open generic DPDK physical lab runner (Debian trixie)

  <Enter>       Start LIVE diagnostics (does not write to disk)
  install       Install via VGA keyboard (selected disk erased after confirmation)
  install-serial Install via serial ttyS0 (selected disk erased after confirmation)

The installer requires the exact confirmation ERASE /dev/<whole-disk>.
It does not automatically select or overwrite a disk.
This lab image enables root autologin on local consoles; do not expose it to
an untrusted network. Configure management mTLS before enabling mgrd.
