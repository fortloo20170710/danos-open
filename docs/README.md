# DANOS-Open Documentation Map

**`docs/` is the single documentation root.** Everything a reader or
contributor needs lives here; everything else is an archive.

## Active documents

| Document | Purpose |
|----------|---------|
| [ADR index](adr/README.md) | Architecture decisions 0001–0007 (read first) |
| [Interop DoD](interop/v0.4_interop_dod.md) | External-interop acceptance (I/V/K groups) |
| [Compat matrix](interop/v0.5_compat_matrix.md) | Frozen protocol versions + upgrade policy |
| [Threading contract](threading.md) | Threads, shared state, lock rules |
| [Release notes](release/) | Per-tag notes (v0.6.0+) |
| [Project status](project-status.md) | Current assessment, roadmap and acceptance policy |
| [Outstanding work plan](outstanding-work-plan.md) | Current release gates and engineering backlog; dated evidence is in the archive |
| [Hardening pass 2026-10-04](security-and-correctness-hardening-2026-10-04.md) | Memory-safety, persistence and VPP wire-protocol fixes |
| [v0.16 acceptance matrix](v0.16-acceptance-matrix.md) | Single current rc1 acceptance record; dated evidence is in the archive |
| [Management resource limits](management-resource-limits.md) | Connection, frame, queue and message budgets for the gNMI listener |
| [NETCONF over SSH](management-netconf-ssh.md) | Deployment, datastores, capabilities, roles and framing limits |
| [v0.16 performance schema](v0.16-performance-result-schema.md) | Machine-readable QEMU/VMware/PCI result fields |
| [v0.16 PCI runner contract](v0.16-pci-runner-contract.md) | External real-DPDK runner prerequisites and first-pass contract |
| [v0.16 next-stage plan](v0.16-next-stage-plan.md) | Release closure sequence and deferred extensions |
| [Coverage baseline](coverage-baseline.md) | Production C coverage measurement, gate and provenance |
| [Physical DPDK perf test plan](physical-dpdk-perf-testplan.md) | testpmd physical baseline procedure (draft runbook) |
| [Architecture diagrams](architecture.md) | Mermaid overview and simplified data flow |

## Entry points outside docs/

- `README.md` — quick start, module map, verified interoperability
- `CONTRIBUTING.md` — gates, rules, where things live
- `TODO.md` — public backlog (environment/feature/quality)
- `tools/perf-testpmd/` — draft physical testpmd harness (not acceptance evidence)
- `tools/gnmic/` — hash-verified Go module for the CI gnmic dependency

## Archive

- `archive/v1.1/` — the frozen v1.1 architecture specification set
  (design-era documents; superseded in part by the ADRs and the
  interop/compat records above). Do not update; cite as history.
- `archive/outstanding-work-plan-history.md` — dated plan checkpoints through
  2026-10-13.
- `archive/v0.16-acceptance-matrix-history.md` — dated acceptance evidence
  through 2026-10-09.

## Rules

1. New design decisions → `docs/adr/NNNN-*.md` (no other location).
2. New acceptance criteria → extend `docs/interop/v0.4_interop_dod.md`
   or a new dated file in `docs/interop/`.
3. Anything frozen gets moved under `docs/archive/` with a note in the
   doc map above. No parallel "current" copies.
