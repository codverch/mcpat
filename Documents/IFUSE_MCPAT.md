# I-Fuse McPAT model

Models the five set-associative I-Fuse tables of Scarab
`hpca2027-revision-ifuse` as CACTI cache-mode arrays (tag array + data array)
plus a tree-PLRU state RAM. McPAT reports area, access time, read/write
energy, leakage, and runtime power for each table.

## Default organization

| Table | Stage | Sets x ways | Tag b | Data b | Repl b/set | Ports |
|-------|-------|-------------|-------|--------|------------|-------|
| FCT | decode | 128 x 4 | 32 | 69 | 3 (PLRU) | 6R/1W |
| APT | decode, rename | 32 x 8 | 43 | 21 | 7 (PLRU) | 6R/6W |
| ACI | execute | 64 x 4 | 36 | 20 | 3 (PLRU) | 1R/1W |
| TT | retire | 32 x 4 | 32 | 68 (incl. RRPV) | 0 | 1R/1W |
| RLB | retire | 16 x 8 | 28 | 65 (incl. timestamp) | 0 | 1R/1W |

FCT and APT get one read port per rename slot (worst case). Any field can be
overridden in XML as `<table>_<field>`, e.g. `fct_rd_ports`.

## Running on Scarab results

```bash
make -j
python3 scripts/ifuse_power/run_ifuse_power.py --out results/hpca2027-revision
python3 scripts/ifuse_power/summarize.py --out results/hpca2027-revision
```

The run script reads each run's `mcpat_infile.xml` and `ifuse.stat.0.out`
from the Scarab git branches, adds the IFuse component, and adds back the
work Scarab's power interface drops for each fused LD2 (rename, PRF write,
wakeup, address check). See the script header for the exact list.

## CACTI note

`cacti/const.h` sets `MINSUBARRAYROWS` to 8 (was 16). Scarab's Golden Cove
config has 8-bank L1 caches, which leave 8 rows per bank. With 16, CACTI finds
no valid organization. The value 8 reproduces the McPAT binary used for the
committed Scarab power results byte for byte.
