#!/usr/bin/env python3
"""Install an exact ISO only onto a newly created disposable QEMU disk."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "live"))
from verify_usb_keyboard_qemu import send_text


def reboot_login_ready(raw, offset):
    fresh = raw[offset:]
    kernel = fresh.find("Linux version ")
    return kernel >= 0 and "root@danos-dpdk-runner:" in fresh[kernel:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("iso", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--mgrd-sha256", required=True)
    p.add_argument("--installed-disk", type=Path,
                   help="read-only backing disk with matching successful installation evidence")
    p.add_argument("--management-smoke-script", type=Path,
                   help="project mTLS test run inside the disposable guest, with ephemeral PKI")
    p.add_argument("--management-vpp-readonly", action="store_true",
                   help="require real guest VPP connection; only operational reads and rejected state writes")
    p.add_argument("--management-candidate-mgrd", type=Path,
                   help="test a separate candidate from the read-only seed, without replacing installed mgrd")
    p.add_argument("--management-vpp-route-lifecycle", action="store_true",
                   help="mutate only the disposable guest: authenticated route add/withdraw and independent FIB")
    a = p.parse_args()
    if a.management_vpp_readonly and not a.management_smoke_script:
        p.error("--management-vpp-readonly requires --management-smoke-script")
    if a.management_candidate_mgrd and not a.management_vpp_readonly:
        p.error("candidate testing requires --management-vpp-readonly")
    if a.management_vpp_route_lifecycle and not a.management_candidate_mgrd:
        p.error("route lifecycle requires explicit candidate binary")
    iso = a.iso.resolve(strict=True)
    out = a.out.resolve()
    if out.exists() or len(a.mgrd_sha256) != 64 or any(c not in "0123456789abcdef" for c in a.mgrd_sha256):
        p.error("new output directory and exact binary SHA256 required")
    out.mkdir(parents=True)
    disk = out / "target.qcow2"
    iso_digest = hashlib.file_digest(iso.open("rb"), "sha256").hexdigest()
    seed = None
    if a.management_smoke_script:
        script = a.management_smoke_script.resolve(strict=True)
        seed_dir = out / "management-seed"
        seed_dir.mkdir()
        shutil.copyfile(script, seed_dir / "test_mtls.py")
        if a.management_candidate_mgrd:
            candidate = a.management_candidate_mgrd.resolve(strict=True)
            shutil.copyfile(candidate, seed_dir / "candidate-mgrd")
            (seed_dir / "candidate-mgrd").chmod(0o755)
        seed = out / "management-seed.iso"
        subprocess.run(["xorriso", "-as", "mkisofs", "-o", str(seed), str(seed_dir)], check=True)
    if a.installed_disk:
        source = a.installed_disk.resolve(strict=True)
        prior = json.loads((source.parent / "result.json").read_text())
        if not source.is_file() or prior.get("installation") != "PASS" or prior.get("iso_sha256") != iso_digest or prior.get("mgrd_sha256") != a.mgrd_sha256:
            p.error("backing disk lacks matching installation evidence")
        subprocess.run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2", "-b", str(source), str(disk)], check=True)
    else:
        subprocess.run(["qemu-img", "create", "-f", "qcow2", str(disk), "5G"], check=True)
    token = uuid.uuid4().hex
    result = dict(scope="qemu-bios-install-coldboot-reboot", status="FAIL",
                  iso=str(iso), iso_sha256=iso_digest,
                  mgrd_sha256=a.mgrd_sha256, hardware_status="ENVIRONMENT-OPEN")
    proc = None
    connection = None
    reader = None
    log = None
    try:
        def launch(name, with_iso):
            nonlocal proc, connection, reader, log
            serial, monitor = out / (name + ".sock"), out / (name + ".monitor")
            log = out / (name + ".serial.log")
            args = ["qemu-system-x86_64", "-enable-kvm", "-machine", "pc", "-cpu", "host",
                    "-m", "2048", "-smp", "2", "-display", "none", "-vga", "std",
                    "-drive", f"file={disk},format=qcow2,if=virtio",
                    "-serial", f"unix:{serial},server=on,wait=off",
                    "-monitor", f"unix:{monitor},server=on,wait=off",
                    "-netdev", "user,id=net", "-device", "e1000,netdev=net"]
            if with_iso:
                args += ["-device", "qemu-xhci,id=usb", "-drive",
                         f"if=none,id=iso,file={iso},format=raw,readonly=on",
                         "-device", "usb-storage,drive=iso,bus=usb.0,bootindex=1"]
            elif seed:
                args += ["-drive", f"file={seed},format=raw,if=virtio,readonly=on"]
            proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=(out / (name + ".qemu.log")).open("wb"))
            deadline = time.monotonic() + 10
            while not serial.exists():
                if proc.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("QEMU failed to open serial socket")
                time.sleep(.1)
            connection = socket.socket(socket.AF_UNIX)
            connection.connect(str(serial))
            def capture(conn, target):
                with target.open("wb") as stream:
                    while True:
                        try:
                            data = conn.recv(65536)
                        except OSError:
                            break
                        if not data:
                            break
                        stream.write(data)
                        stream.flush()
            reader = threading.Thread(target=capture, args=(connection, log), daemon=True)
            reader.start()
            return monitor

        def wait(text, timeout=120, offset=0):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                raw = log.read_text(errors="replace") if log.exists() else ""
                if text in raw[offset:]:
                    return
                if proc.poll() is not None:
                    raise RuntimeError("QEMU exited; " + raw[-2000:])
                time.sleep(.2)
            raise TimeoutError("waiting for " + text + "; " + raw[-2000:])

        def stop():
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            if connection is not None:
                connection.close()
            if reader is not None:
                reader.join(timeout=2)

        if a.installed_disk:
            result.update(installation="PRIOR-PASS", installation_evidence=str(source.parent / "result.json"),
                          scope="qemu-installed-overlay-coldboot-reboot")
        else:
            monitor = launch("install", True)
            time.sleep(8)  # BIOS/ISOLINUX VGA prompt, not a direct kernel boot.
            send_text(str(monitor), "install-serial\n")
            wait("Enter the exact target whole-disk path", 180)
            connection.sendall(b"/dev/vda\n")
            wait("Type exactly ERASE /dev/vda to confirm")
            connection.sendall(b"ERASE /dev/vda\n")
            wait("DANOS-INSTALLER-COMPLETE", 240)
            raw = log.read_text(errors="replace")
            if "DANOS INSTALLER PASS" not in raw or "DANOS-INSTALLER-EXIT rc=0" not in raw:
                raise ValueError("installer success/readback markers absent")
            result["installation"] = "PASS"
            stop()
        launch("disk", False)  # ISO genuinely absent.
        wait("root@danos-dpdk-runner:", 180)
        command = ("systemctl is-active --quiet vpp && systemctl is-active --quiet frr && "
                   "test -S /run/vpp/api.sock && test -S /run/vpp/stats.sock && "
                   "vppctl -s /run/vpp/cli.sock show version && "
                   "ldd /usr/bin/danos-mgrd >/tmp/r13-mgrd-ldd && ! grep -q 'not found' /tmp/r13-mgrd-ldd && "
                   f"test \"$(sha256sum /usr/bin/danos-mgrd | cut -d' ' -f1)\" = {a.mgrd_sha256} && "
                   "test \"$(findmnt -n -o SOURCE /)\" = /dev/vda1 && "
                   "test \"$(findmnt -n -o FSTYPE /)\" = ext4 && "
                   "grep -qx DANOS_BUILD_SOURCE_DIRTY=0 /etc/danos/build-info.env && "
                   "! systemctl is-active --quiet danos-mgrd && "
                   "findmnt -n -o SOURCE,FSTYPE / && cat /etc/danos/build-info.env && "
                   f"printf '{token}\\n' >/etc/danos/r13-persistence-marker && sync && printf '\\nINSTALLED_RUNTIME_PASS\\n'\n")
        connection.sendall(command.encode())
        wait("\nINSTALLED_RUNTIME_PASS\n", 120)
        result["coldboot"] = "PASS"
        if seed:
            smoke_offset = len(log.read_text(errors="replace"))
            connection.sendall(("mkdir -p /mnt/management-smoke && mount -o ro /dev/vdb /mnt/management-smoke && "
                                "timeout 180 python3 /mnt/management-smoke/test_mtls.py "
                                + ("/mnt/management-smoke/candidate-mgrd " if a.management_candidate_mgrd else "/usr/bin/danos-mgrd ")
                                + "/usr/bin/openssl "
                                + ("--vpp-route-lifecycle " if a.management_vpp_route_lifecycle else ("--vpp-programming-readonly " if a.management_candidate_mgrd else ("--vpp-readonly " if a.management_vpp_readonly else ""))) +
                                ">/tmp/management-smoke.log 2>&1; smoke_rc=$?; cat /tmp/management-smoke.log; "
                                "if test $smoke_rc -eq 0 && ! systemctl is-active --quiet danos-mgrd; then "
                                f"printf '\\nMANAGEMENT_SMOKE_{token}_PASS\\n'; fi\n").encode())
            wait(f"\nMANAGEMENT_SMOKE_{token}_PASS\n", 210, smoke_offset)
            result.update(management_mtls="PASS", management_scope="guest-loopback --no-vpp; ephemeral PKI; lab opt-in tested separately; no physical backend qualification",
                          management_script_sha256=hashlib.file_digest(script.open("rb"), "sha256").hexdigest())
            if a.management_vpp_readonly:
                result["management_scope"] = "guest-loopback real VPP API connection; ephemeral PKI; operational Get and rejected state writes only; no route/FIB or physical qualification"
            if a.management_candidate_mgrd:
                result.update(management_candidate_sha256=hashlib.file_digest(candidate.open("rb"), "sha256").hexdigest(),
                              management_scope="separate candidate on read-only seed; explicit VPP programming backend activation and mTLS operational reads only; installed binary unchanged; no route/FIB or physical qualification")
            if a.management_vpp_route_lifecycle:
                result.update(management_scope="separate candidate in disposable guest: authenticated route add/withdraw, ledger attribution and independent VPP FIB; no physical or packet/performance qualification",
                              management_route_lifecycle="PASS")
        offset = len(log.read_text(errors="replace"))
        connection.sendall(b"reboot\n")
        wait("Linux version ", 180, offset)
        reboot_offset = log.read_text(errors="replace").rfind("Linux version ")
        wait("root@danos-dpdk-runner:", 180, reboot_offset)
        if not reboot_login_ready(log.read_text(errors="replace"), offset):
            raise ValueError("no fresh kernel boot and login after reboot request")
        connection.sendall((f"test \"$(cat /etc/danos/r13-persistence-marker)\" = {token} && systemctl is-active --quiet vpp && systemctl is-active --quiet frr && test -S /run/vpp/api.sock && test -S /run/vpp/stats.sock && ! systemctl is-active --quiet danos-mgrd && printf '\\nPERSISTENCE_RUNTIME_PASS\\n'\n").encode())
        wait("\nPERSISTENCE_RUNTIME_PASS\n", 120, offset)
        result.update(status="PASS", reboot="PASS")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        result["failure_reason"] = str(exc)
    finally:
        if proc is not None:
            stop()
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
