#!/bin/busybox sh
# Install the packaged persistent generic DPDK lab-runner disk image. The default path
# is interactive and refuses mounted disks and the media currently booted.
set -eu

media_mount=${DANOS_INSTALLER_MEDIA_MOUNT:-/cdrom}
sysfs_root=${DANOS_INSTALLER_SYSFS_ROOT:-/sys}
mounts_file=${DANOS_INSTALLER_MOUNTS_FILE:-/proc/mounts}
console=${DANOS_INSTALLER_CONSOLE:-/dev/console}
test_mode=${DANOS_INSTALL_TEST_MODE:-0}
test_target_root=${DANOS_INSTALL_TEST_TARGET_ROOT:-}
media_wait_seconds=${DANOS_INSTALLER_MEDIA_WAIT_SECONDS:-45}
payload="$media_mount/installer/danos-runner-installed.raw.gz"
metadata="$media_mount/installer/disk-image.env"
if test ! -r "$payload"; then
    # Compatibility with the QEMU-qualified base ISO's original payload path.
    payload="$media_mount/installer/danos-i211-installed.raw.gz"
fi

say() {
    echo "$*" >> "$console" 2>/dev/null || true
    echo "$*" >> /dev/ttyS0 2>/dev/null || true
}

read_line() {
    if test "${DANOS_INSTALLER_INPUT:-console}" = serial; then
        IFS= read -r REPLY </dev/ttyS0
    else
        IFS= read -r REPLY
    fi
}

if test "$test_mode" != 1; then
    say 'DANOS INSTALLER: loading persistent disk image from the boot ISO'
    mkdir -p "$media_mount"
    media_device=
    media_attempt=0
    while test "$media_attempt" -lt "$media_wait_seconds"; do
        media_attempt=$((media_attempt + 1))
        # USB mass-storage SCSI discovery is asynchronous: the initramfs can
        # enter this installer before the boot stick has become /dev/sdX.
        mdev -s 2>/dev/null || true
        for candidate in /dev/sr* /dev/sd* /dev/vd* /dev/nvme*n* /dev/mmcblk*; do
            test -b "$candidate" || continue
            if mount -t iso9660 -o ro "$candidate" "$media_mount" 2>/dev/null; then
                if test -r "$payload" && test -r "$metadata"; then
                    media_device=$candidate
                    break
                fi
                umount "$media_mount" 2>/dev/null || true
            fi
        done
        test -n "$media_device" && break
        if test $((media_attempt % 5)) -eq 0; then
            say "DANOS INSTALLER: waiting for USB/ISO block media (${media_attempt}/${media_wait_seconds}s)"
        fi
        sleep 1
    done
    test -n "$media_device" || {
        say 'DANOS INSTALLER FAIL: cannot locate installer payload on ISO media'
        exit 1
    }
else
    media_device=test-media
fi

test -r "$payload" && test -r "$metadata" || {
    say 'DANOS INSTALLER FAIL: disk image payload/manifest is missing'
    exit 1
}
. "$metadata"
case "${DANOS_INSTALLED_DISK_BYTES:-}" in
    ''|*[!0-9]*) say 'DANOS INSTALLER FAIL: invalid disk image size'; exit 1 ;;
esac
case "${DANOS_INSTALLED_DISK_SHA256:-}" in
    *[!0123456789abcdefABCDEF]*|'') say 'DANOS INSTALLER FAIL: invalid disk SHA256'; exit 1 ;;
esac
test "${#DANOS_INSTALLED_DISK_SHA256}" -eq 64 || {
    say 'DANOS INSTALLER FAIL: disk SHA256 must contain 64 hex digits'
    exit 1
}
test $((DANOS_INSTALLED_DISK_BYTES % 512)) -eq 0 || {
    say 'DANOS INSTALLER FAIL: disk image size is not sector aligned'
    exit 1
}
if test -n "${DANOS_INSTALLED_DISK_GZIP_SHA256:-}"; then
    actual_gzip_sha=$(sha256sum "$payload" | awk '{print $1}')
    test "$actual_gzip_sha" = "$DANOS_INSTALLED_DISK_GZIP_SHA256" || {
        say 'DANOS INSTALLER FAIL: compressed payload digest mismatch'
        exit 1
    }
fi
gzip -t "$payload" || {
    say 'DANOS INSTALLER FAIL: compressed payload is corrupt'
    exit 1
}

mounted_disk() {
    candidate_name=$1
    while IFS=' ' read -r source mountpoint rest; do
        case "$source" in
            /dev/*)
                source_name=${source##*/}
                test "$source_name" = "$candidate_name" && return 0
                case "$source_name" in
                    "$candidate_name"p[0-9]*|"$candidate_name"[0-9]*) return 0 ;;
                esac
                ;;
        esac
    done < "$mounts_file"
    return 1
}

mounted_media_disk() {
    candidate_name=$1
    media_name=${media_device##*/}
    test "$media_name" = "$candidate_name" && return 0
    case "$media_name" in
        "$candidate_name"p[0-9]*|"$candidate_name"[0-9]*) return 0 ;;
    esac
    return 1
}

disk_size_bytes() {
    candidate_name=$1
    sysdisk="$sysfs_root/block/$candidate_name"
    test -r "$sysdisk/size" || return 1
    sectors=$(cat "$sysdisk/size")
    case "$sectors" in ''|*[!0-9]*) return 1 ;; esac
    echo $((sectors * 512))
}

test "$test_mode" = 1 || {
    say 'DANOS-OPEN INSTALLER — GENERIC DPDK PHYSICAL LAB RUNNER'
    say 'WARNING: the selected whole disk will be completely overwritten.'
    say 'This installs a lab image with root autologin on physical consoles.'
    say 'Do not install on a disk containing data you need to keep.'
    say 'Available whole disks:'
    for sysdisk in "$sysfs_root"/block/*; do
        test -r "$sysdisk/size" || continue
        name=${sysdisk##*/}
        case "$name" in loop*|ram*|fd*|sr*|zram*|dm-*|md*) continue ;; esac
        mounted_disk "$name" && continue
        mounted_media_disk "$name" && continue
        for holder in "$sysdisk"/holders/*; do
            test -e "$holder" && continue 2
        done
        bytes=$(disk_size_bytes "$name") || continue
        model=$(cat "$sysdisk/device/model" 2>/dev/null || echo unknown-model)
        removable=$(cat "$sysdisk/removable" 2>/dev/null || echo 0)
        say "  /dev/$name size=$bytes model=$model removable=$removable"
    done
}

if test "$test_mode" = 1; then
    test -n "$test_target_root" || { say 'test mode requires a target root'; exit 2; }
    say "Test target file under $test_target_root:"
else
    say 'Enter the exact target whole-disk path (for example /dev/sda):'
fi
read_line || exit 1
target=$REPLY
case "$target" in ''|*' '*) say 'DANOS INSTALLER FAIL: invalid target path'; exit 1 ;; esac

if test "$test_mode" = 1; then
    case "$target" in "$test_target_root"/*) ;; *) say 'DANOS INSTALLER FAIL: test target is outside test root'; exit 1 ;; esac
    test -f "$target" || { say 'DANOS INSTALLER FAIL: test target is not a regular file'; exit 1; }
    target_name=${target##*/}
else
    case "$target" in /dev/*) ;; *) say 'DANOS INSTALLER FAIL: target must be a /dev whole disk'; exit 1 ;; esac
    test -b "$target" || { say 'DANOS INSTALLER FAIL: target is not a block device'; exit 1; }
    target_name=${target#/dev/}
fi

sysdisk="$sysfs_root/block/$target_name"
if test "$test_mode" != 1; then
    test -d "$sysdisk" || { say 'DANOS INSTALLER FAIL: target is not a whole disk'; exit 1; }
    mounted_disk "$target_name" && { say 'DANOS INSTALLER FAIL: target has mounted filesystems'; exit 1; }
    mounted_media_disk "$target_name" && { say 'DANOS INSTALLER FAIL: refusing to overwrite boot media'; exit 1; }
    for holder in "$sysdisk"/holders/*; do
        test -e "$holder" && { say 'DANOS INSTALLER FAIL: target is in use by a mapped device'; exit 1; }
    done
fi

available_bytes=$(disk_size_bytes "$target_name") || {
    if test "$test_mode" = 1; then
        available_bytes=$(stat -c '%s' "$target")
    else
        say 'DANOS INSTALLER FAIL: cannot determine target capacity'
        exit 1
    fi
}
test "$available_bytes" -ge "$DANOS_INSTALLED_DISK_BYTES" || {
    say "DANOS INSTALLER FAIL: target is too small ($available_bytes < $DANOS_INSTALLED_DISK_BYTES)"
    exit 1
}

say "Selected target: $target"
say "Image bytes: $DANOS_INSTALLED_DISK_BYTES"
say "Type exactly ERASE $target to confirm:"
read_line || exit 1
confirmation=$REPLY
test "$confirmation" = "ERASE $target" || {
    say 'DANOS INSTALLER ABORTED: confirmation did not match; target untouched'
    exit 1
}

say 'DANOS INSTALLER: writing persistent system image; do not power off'
gzip -dc "$payload" | dd of="$target" bs=4M iflag=fullblock conv=fsync status=noxfer
sync
say 'DANOS INSTALLER: write finished; verifying target readback digest'
# Large reads avoid millions of 512-byte operations. Hash exactly the image
# length, including a sector-aligned tail, not extra bytes on a larger disk.
readback_chunks=$((DANOS_INSTALLED_DISK_BYTES / 4194304))
readback_tail_sectors=$(((DANOS_INSTALLED_DISK_BYTES % 4194304) / 512))
actual_disk_sha=$({
    dd if="$target" bs=4M count="$readback_chunks" 2>/dev/null
    if test "$readback_tail_sectors" -gt 0; then
        dd if="$target" bs=512 skip=$((readback_chunks * 8192)) count="$readback_tail_sectors" 2>/dev/null
    fi
} | sha256sum | awk '{print $1}')
test "$actual_disk_sha" = "$DANOS_INSTALLED_DISK_SHA256" || {
    say 'DANOS INSTALLER FAIL: target readback digest mismatch'
    exit 1
}
sync
say 'DANOS INSTALLER PASS: disk image written and readback digest verified'
say 'Remove the USB/CD media, then cold boot from the installed disk.'
