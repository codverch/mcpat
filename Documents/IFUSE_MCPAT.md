# I-Fuse McPAT Integration

This branch adds area and power modeling for the runtime I-Fuse hardware
structures used in Scarab (`hpca2027-runtime-ifuse`).

## Modeled structures

| Structure | Stage | Organization | Default size |
|-----------|-------|--------------|--------------|
| RLB (Retired Load Buffer) | Retire | Direct-mapped SRAM | 512 entries |
| TT (Training Table) | Retire | Set-assoc + tree PLRU | 32×4 entries |
| FCT (Fusion Candidate Table) | Fetch | Direct-mapped SRAM | 512 entries |
| APT (Active Pair Table) | Fetch | Set-assoc + tree PLRU | 64×4 entries |
| ACI (Access Check Index) | Fetch | Set-assoc + tree PLRU | 64×4 entries |

Entry bit widths are derived from Scarab's hardware packing model:

- `pc_tag_bits` (default 32) scales RLB, FCT, TT, and APT PC fields.
- `training_table_block_tag_bits` (default 20) scales TT block tags.
- **`training_insert_threshold` is policy-only** — it does not change TT storage.
  The observation counter is always modeled as **14 bits** (`training_observation_counter_bits`).

## XML configuration

Enable I-Fuse on a core with:

```xml
<param name="number_of_ifuse" value="1"/>
<component id="system.core0.IFuse" name="IFuse">
  ...
</component>
```

See `ProcessorDescriptionFiles/IFuse_example_snippet.xml` for a complete example.

### Parameters

| Parameter | Default | Notes |
|-----------|---------|-------|
| `enabled` | 0 | Must be 1 (or set `number_of_ifuse`) |
| `pc_tag_bits` | 32 | Shared with Scarab `--ifuse_pc_tag_bits` |
| `rlb_entries` | 512 | Direct-mapped RLB capacity |
| `fct_entries` | 512 | FCT rows (`2^ifuse_fct_hash_bits`) |
| `training_table_sets` | 32 | TT sets (×4 ways = total entries) |
| `training_table_ways` | 4 | TT associativity |
| `training_insert_threshold` | 1000 | Promotion policy; **not** counter width |
| `training_table_block_tag_bits` | 20 | Partial cache-block tag in TT |
| `training_observation_counter_bits` | 14 | Fixed hardware counter width |
| `apt_sets` / `apt_ways` | 64 / 4 | APT geometry |
| `aci_sets` / `aci_ways` | 64 / 4 | ACI geometry |

### Runtime statistics (from Scarab)

| Stat | Scarab counter source |
|------|----------------------|
| `rlb_read_accesses` / `rlb_write_accesses` | Retired on-path loads |
| `fct_read_accesses` | `FCT_LOOKUP_HITS + FCT_LOOKUP_MISSES` |
| `fct_write_accesses` | `FCT_RUNTIME_INSERTS + FCT_RUNTIME_REINFORCEMENTS + replacements` |
| `tt_read_accesses` | `TRAINING_TABLE_LOOKUPS` |
| `tt_write_accesses` | `TRAINING_TABLE_OBSERVATIONS` |
| `apt_read_accesses` | `APT_LOOKUP_HITS + APT_LOOKUP_MISSES` |
| `apt_write_accesses` | `APT_INSERTS + APT_EVICTIONS` |
| `aci_read_accesses` | `ACI_LOOKUP_HITS + ACI_LOOKUP_MISSES` |
| `aci_write_accesses` | `ACI_PREDICTION_INSERTS + ACI_REPLAYED_INSERTS` |

## Build and run

```bash
make -j
./mcpat -infile ProcessorDescriptionFiles/N1.xml -print_level 2
```

With I-Fuse enabled in the XML, McPAT reports an **I-Fuse (runtime load fusion)** section
under each core, including area, leakage, and dynamic power.

## Scarab integration

Point `MCPAT_BIN` at this build and enable Scarab's power interface
(`--power_intf_on 1`). Scarab's `power_scarab_config.cc` emits the IFuse XML block
when runtime I-Fuse is enabled.
