import unittest
from verify_installable_qemu import reboot_login_ready


class RebootEvidenceTests(unittest.TestCase):
    def test_old_shell_prompt_cannot_prove_reboot(self):
        self.assertFalse(reboot_login_ready("reboot\nroot@danos-dpdk-runner:~# ", 0))

    def test_kernel_and_login_must_be_fresh_and_ordered(self):
        previous = "Linux version old\nroot@danos-dpdk-runner:~# \n"
        self.assertFalse(reboot_login_ready(previous, len(previous)))
        self.assertFalse(reboot_login_ready("root@danos-dpdk-runner:~# \nLinux version new\n", 0))
        self.assertTrue(reboot_login_ready(previous + "Linux version new\nroot@danos-dpdk-runner:~# ", len(previous)))


if __name__ == "__main__":
    unittest.main()
