#!/usr/bin/env python3
import unittest
from check_physical_pci_id import physical_id


class PhysicalIdentity(unittest.TestCase):
    def test_i211(self):
        self.assertEqual(physical_id('0x8086', '0x1539'), '8086:1539')

    def test_virtual_and_malformed(self):
        for vendor, device in [('0x15ad', '0x07b0'), ('1af4', '1000'), ('8086', '100e'),
                               ('8086', '100f'), ('8086', '10d3'), ('1234', '1111'),
                               ('', ''), ('8086', '../1539')]:
            with self.subTest(vendor=vendor, device=device), self.assertRaises(ValueError):
                physical_id(vendor, device)


if __name__ == '__main__':
    unittest.main()
