# testpmd physical-machine performance draft

Status: **draft, not acceptance evidence.** These helpers were collected while
preparing the 4×Intel I211 testpmd runs described in
[`docs/physical-dpdk-perf-testplan.md`](../../docs/physical-dpdk-perf-testplan.md).
No result produced with them has been reviewed or added to the release
acceptance record, and the `ENVIRONMENT-OPEN` PCI/DPDK limitation remains.

Known gaps before any result can count:

- `parse_testpmd.py` expects counter lines that normal testpmd output does not
  emit verbatim; the parser needs to be checked against a real log before its
  JSON is trusted.
- The scripts assume `./testpmd-static/bin/testpmd-static`, BDFs
  `0000:01:00.0`/`0000:02:00.0`, one core and `uio_pci_generic`; edit the
  configuration block on each machine.
- The collector embeds lab addresses (`192.168.1.244`–`192.168.1.247`);
  override them with arguments.

## Files

| File | Purpose |
| --- | --- |
| `Dockerfile.testpmd`, `build_testpmd.sh` | Build a static testpmd (igb PMD only) in a Debian container |
| `create_testpmd_deploy.sh`, `create_deploy_package.sh` | Assemble a deployable tarball |
| `run_tx.sh`, `run_rx.sh`, `run_ecmp.sh` | testpmd txonly/rxonly/ECMP distribution runs |
| `collect_results.sh` | Gather logs and environment details from the lab hosts |
| `parse_testpmd.py` | Turn testpmd logs into a JSON summary |

Run the scripts from a deployment directory that contains `testpmd-static/`,
as described in the test plan.
