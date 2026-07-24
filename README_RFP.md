# McPAT RFP Power Modeling

This branch adds integrated power/area modeling for **Register File Prefetch (RFP)** PT and PAT SRAM tables (ISCA'22 Table 1) to McPAT, for use with the Scarab `power_intf` pipeline.

## Components

Under each core (`system.coreN`), when `rfp_on=1`:

| XML component | McPAT model | Description |
|---------------|-------------|-------------|
| `system.coreN.rfp_pt` | `ArrayST` (set-assoc SRAM) | Prefetch Table (PC-indexed stride metadata) |
| `system.coreN.rfp_pat` | `ArrayST` (set-assoc SRAM) | Page Address Table (PFN storage) |

Power is included in **Load/Store Unit** totals and rolls up into per-core and chip power.

## XML format

```xml
<param name="rfp_on" value="1"/>
<component id="system.core0.rfp_pt" name="RFP_PT">
  <param name="rfp_config" value="8192,8,8,1,1,1"/>
  <!-- capacity(bytes), line(bytes), assoc, banks, throughput, latency -->
  <stat name="read_accesses" value="..."/>
  <stat name="write_accesses" value="..."/>
</component>
<component id="system.core0.rfp_pat" name="RFP_PAT">
  <param name="rfp_config" value="512,8,4,1,1,1"/>
  <stat name="read_accesses" value="..."/>
  <stat name="write_accesses" value="..."/>
</component>
```

## Storage sweep configs (HPCA'27 / ISCA'22 Table 1)

Entry size: **8 bytes** per PT/PAT line (compressed Table 1 fields rounded up for CACTI).

| Config | PT (sets×ways) | `rfp_config` capacity | PAT (sets×ways) | `rfp_config` capacity |
|--------|----------------|----------------------|-----------------|----------------------|
| rfp_6kb  | 128×8  | 8192  | 16×4  | 512   |
| rfp_12kb | 256×8  | 16384 | 32×4  | 1024  |
| rfp_18kb | 512×6  | 24576 | 32×6  | 1536  |
| rfp_24kb | 512×8  | 32768 | 64×4  | 2048  |

Formula: `capacity = num_sets × num_ways × entry_bytes`

## Scarab integration

1. Build this McPAT branch and point `MCPAT_BIN` at the resulting `mcpat` binary.
2. Run Scarab with `--power_intf_on 1` and RFP enabled (`--rfp_on 1` plus PT/PAT sizing params).
3. Scarab emits `POWER_RFP_PT_{READ,WRITE}` and `POWER_RFP_PAT_{READ,WRITE}` stats into the McPAT XML automatically.

## Build

```bash
make -j$(nproc)
# binary: ./mcpat
```

## References

- ISCA'22 RFP paper, Table 1 (compressed PT/PAT entry fields and storage budgets)
- Scarab `power_scarab_config.cc` → `power_print_core_rfp()`
