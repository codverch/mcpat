/*****************************************************************************
 * McPAT area/power model for runtime I-Fuse tables.
 *****************************************************************************/

#include "ifuse.h"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "const.h"

namespace {

static int ceil_div8(int bits) { return (bits + 7) / 8; }

/* Retired Load Buffer: pc_tag + 33b block + 6b offset + 3b size + 10b uop + valid */
static int rlb_entry_bits(int pc_tag_bits) {
  return pc_tag_bits + 33 + 6 + 3 + 10 + 1;
}

/* FCT row: two PC tags + 6b delta + dir + 3b size + 9b confidence + valid */
static int fct_entry_bits(int pc_tag_bits) {
  return 2 * pc_tag_bits + 6 + 1 + 3 + 9 + 1;
}

/*
 * Training table row: two PC tags + block_tag + 14b observation counter
 * (fixed width; insert_threshold is policy-only) + 6b delta + 3b size + dir + valid.
 */
static int tt_entry_bits(int pc_tag_bits, int block_tag_bits) {
  const int kObsCounterBits = 14;
  return 2 * pc_tag_bits + block_tag_bits + kObsCounterBits + 6 + 3 + 1 + 1;
}

/* APT row: scales with pc_tag_bits; 83 bits at 32-bit tags (51 non-PC payload). */
static int apt_entry_bits(int pc_tag_bits) {
  const int kNonPcBits = 51;
  return 2 * pc_tag_bits + kNonPcBits;
}

/* ACI row: predicted block metadata + LD1 micro-op id (41 bits in Scarab model). */
static int aci_entry_bits() { return 41; }

static int plru_bits_per_set(int ways) {
  if (ways <= 1) return 0;
  if (ways == 4) return 3;
  if (ways == 8) return 7;
  return (int)ceil(log2((double)ways));
}

static void reset_interface_ip(InputParameter* ip) {
  ip->is_cache = false;
  ip->pure_ram = false;
  ip->pure_cam = false;
  ip->specific_tag = 0;
  ip->access_mode = 0;
  ip->obj_func_dyn_energy = 0;
  ip->obj_func_dyn_power = 0;
  ip->obj_func_leak_power = 0;
  ip->obj_func_cycle_t = 1;
  ip->num_se_rd_ports = 0;
}

static ArrayST* make_direct_table(InputParameter* base_ip, const char* name,
                                  int entries, int entry_bytes, int rd_ports,
                                  int wr_ports, double clock_rate,
                                  const CoreDynParam& dyn) {
  InputParameter ip = *base_ip;
  reset_interface_ip(&ip);
  ip.is_cache = false;
  ip.pure_ram = true;
  ip.cache_sz = std::max(1, entries * entry_bytes);
  ip.line_sz = std::max(1, entry_bytes);
  ip.assoc = 1;
  ip.nbanks = 1;
  ip.out_w = ip.line_sz * 8;
  ip.throughput = 1.0 / clock_rate;
  ip.latency = 1.0 / clock_rate;
  ip.num_rd_ports = rd_ports;
  ip.num_wr_ports = wr_ports;
  ip.num_rw_ports = 0;
  return new ArrayST(&ip, name, Core_device, dyn.opt_local, dyn.core_ty);
}

static ArrayST* make_set_assoc_table(InputParameter* base_ip, const char* name,
                                     int sets, int ways, int entry_bytes,
                                     int rd_ports, int wr_ports,
                                     double clock_rate, const CoreDynParam& dyn) {
  InputParameter ip = *base_ip;
  reset_interface_ip(&ip);
  ip.is_cache = true;
  ip.pure_ram = false;
  ip.cache_sz = std::max(1, sets * ways * entry_bytes);
  ip.line_sz = std::max(1, entry_bytes);
  ip.assoc = std::max(1, ways);
  ip.nbanks = 1;
  ip.specific_tag = 1;
  ip.tag_w = entry_bytes * 8;
  ip.out_w = ip.line_sz * 8;
  ip.throughput = 1.0 / clock_rate;
  ip.latency = 1.0 / clock_rate;
  ip.num_rd_ports = rd_ports;
  ip.num_wr_ports = wr_ports;
  ip.num_rw_ports = 0;
  return new ArrayST(&ip, name, Core_device, dyn.opt_local, dyn.core_ty);
}

static ArrayST* make_plru_state(InputParameter* base_ip, const char* name,
                                int sets, int ways, double clock_rate,
                                const CoreDynParam& dyn) {
  int bits = plru_bits_per_set(ways) * sets;
  if (bits <= 0) return NULL;
  int bytes = ceil_div8(bits);
  return make_direct_table(base_ip, name, std::max(1, sets), std::max(1, bytes),
                           1, 1, clock_rate, dyn);
}

static void accumulate_array_power(ArrayST* arr, double read_accesses,
                                   double write_accesses) {
  if (!arr) return;
  arr->stats_t.readAc.access = read_accesses;
  arr->stats_t.writeAc.access = write_accesses;
  arr->power_t.readOp.dynamic +=
      arr->local_result.power.readOp.dynamic * read_accesses +
      write_accesses * arr->local_result.power.writeOp.dynamic;
  arr->power = arr->power_t + arr->local_result.power * pppm_lkg;
}

}  // namespace

IFuseU::IFuseU(ParseXML* XML_interface, int ithCore_,
               InputParameter* interface_ip_, const CoreDynParam& dyn_p_,
               bool exist_)
    : XML(XML_interface),
      ithCore(ithCore_),
      interface_ip(*interface_ip_),
      coredynp(dyn_p_),
      exist(exist_),
      rlb(0),
      fct(0),
      training_table(0),
      apt(0),
      aci(0),
      tt_plru(0),
      apt_plru(0),
      aci_plru(0) {
  if (!exist) return;

  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;
  if (cfg.enabled <= 0 && XML->sys.core[ithCore].number_of_ifuse <= 0) {
    exist = false;
    return;
  }

  const int pc_tag_bits = std::max(1, cfg.pc_tag_bits);
  const int rlb_entries = std::max(1, cfg.rlb_entries);
  const int fct_entries = std::max(1, cfg.fct_entries);
  const int tt_sets = std::max(1, cfg.training_table_sets);
  const int tt_ways = std::max(1, cfg.training_table_ways);
  const int apt_sets = std::max(1, cfg.apt_sets);
  const int apt_ways = std::max(1, cfg.apt_ways);
  const int aci_sets = std::max(1, cfg.aci_sets);
  const int aci_ways = std::max(1, cfg.aci_ways);
  const int block_tag_bits = std::max(0, cfg.training_table_block_tag_bits);

  const int rlb_bytes = ceil_div8(rlb_entry_bits(pc_tag_bits));
  const int fct_bytes = ceil_div8(fct_entry_bits(pc_tag_bits));
  const int tt_bytes = ceil_div8(tt_entry_bits(pc_tag_bits, block_tag_bits));
  const int apt_bytes = ceil_div8(apt_entry_bits(pc_tag_bits));
  const int aci_bytes = ceil_div8(aci_entry_bits());

  const double clock_rate = coredynp.clockRate;
  const int fetch_ports = std::max(1, coredynp.fetchW);
  const int retire_ports = std::max(1, coredynp.commitW);

  rlb = make_direct_table(&interface_ip, "IFuse Retired Load Buffer", rlb_entries,
                          rlb_bytes, 1, 1, clock_rate, coredynp);
  area.set_area(area.get_area() + rlb->local_result.area);

  fct = make_direct_table(&interface_ip, "IFuse Fusion Candidate Table",
                          fct_entries, fct_bytes, fetch_ports, 1, clock_rate,
                          coredynp);
  area.set_area(area.get_area() + fct->local_result.area);

  training_table = make_set_assoc_table(
      &interface_ip, "IFuse Training Table", tt_sets, tt_ways, tt_bytes, 1, 1,
      clock_rate, coredynp);
  area.set_area(area.get_area() + training_table->local_result.area);
  tt_plru = make_plru_state(&interface_ip, "IFuse Training Table PLRU", tt_sets,
                            tt_ways, clock_rate, coredynp);
  if (tt_plru) area.set_area(area.get_area() + tt_plru->local_result.area);

  apt = make_set_assoc_table(&interface_ip, "IFuse Active Pair Table", apt_sets,
                             apt_ways, apt_bytes, fetch_ports, 1, clock_rate,
                             coredynp);
  area.set_area(area.get_area() + apt->local_result.area);
  apt_plru = make_plru_state(&interface_ip, "IFuse APT PLRU", apt_sets, apt_ways,
                             clock_rate, coredynp);
  if (apt_plru) area.set_area(area.get_area() + apt_plru->local_result.area);

  aci = make_set_assoc_table(&interface_ip, "IFuse Access Check Index", aci_sets,
                             aci_ways, aci_bytes, fetch_ports, 1, clock_rate,
                             coredynp);
  area.set_area(area.get_area() + aci->local_result.area);
  aci_plru = make_plru_state(&interface_ip, "IFuse ACI PLRU", aci_sets, aci_ways,
                              clock_rate, coredynp);
  if (aci_plru) area.set_area(area.get_area() + aci_plru->local_result.area);

  (void)cfg.training_insert_threshold;
}

void IFuseU::computeEnergy(bool is_tdp) {
  if (!exist) return;

  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;
  power.reset();
  rt_power.reset();

  double rlb_reads = 0;
  double rlb_writes = 0;
  double fct_reads = 0;
  double fct_writes = 0;
  double tt_reads = 0;
  double tt_writes = 0;
  double apt_reads = 0;
  double apt_writes = 0;
  double aci_reads = 0;
  double aci_writes = 0;
  double plru_updates = 0;

  if (is_tdp) {
    const double load_activity = std::max(0.0, coredynp.LSU_duty_cycle);
    rlb_reads = load_activity;
    rlb_writes = load_activity;
    fct_reads = load_activity;
    fct_writes = load_activity * 0.01;
    tt_reads = load_activity * 0.05;
    tt_writes = load_activity * 0.05;
    apt_reads = load_activity;
    apt_writes = load_activity * 0.1;
    aci_reads = load_activity;
    aci_writes = load_activity * 0.1;
    plru_updates = load_activity * 0.1;
  } else {
    rlb_reads = cfg.rlb_read_accesses;
    rlb_writes = cfg.rlb_write_accesses;
    fct_reads = cfg.fct_read_accesses;
    fct_writes = cfg.fct_write_accesses;
    tt_reads = cfg.tt_read_accesses;
    tt_writes = cfg.tt_write_accesses;
    apt_reads = cfg.apt_read_accesses;
    apt_writes = cfg.apt_write_accesses;
    aci_reads = cfg.aci_read_accesses;
    aci_writes = cfg.aci_write_accesses;
    plru_updates = cfg.tt_write_accesses + cfg.apt_write_accesses +
                   cfg.aci_write_accesses;
  }

  accumulate_array_power(rlb, rlb_reads, rlb_writes);
  accumulate_array_power(fct, fct_reads, fct_writes);
  accumulate_array_power(training_table, tt_reads, tt_writes);
  accumulate_array_power(apt, apt_reads, apt_writes);
  accumulate_array_power(aci, aci_reads, aci_writes);
  accumulate_array_power(tt_plru, plru_updates, plru_updates);
  accumulate_array_power(apt_plru, plru_updates, plru_updates);
  accumulate_array_power(aci_plru, plru_updates, plru_updates);

  if (rlb) power = power + rlb->power;
  if (fct) power = power + fct->power;
  if (training_table) power = power + training_table->power;
  if (apt) power = power + apt->power;
  if (aci) power = power + aci->power;
  if (tt_plru) power = power + tt_plru->power;
  if (apt_plru) power = power + apt_plru->power;
  if (aci_plru) power = power + aci_plru->power;

  if (!is_tdp) rt_power = power;
}

void IFuseU::displayEnergy(uint32_t indent, int plevel, bool is_tdp) {
  if (!exist) return;
  std::string indent_str(indent, ' ');
  std::string indent_str_next(indent + 2, ' ');
  bool long_channel = XML->sys.longer_channel_device;
  bool power_gating = XML->sys.power_gating;
  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;

  if (is_tdp) {
    cout << indent_str << "I-Fuse (runtime load fusion):" << endl;
    cout << indent_str_next << "Area = " << area.get_area() * 1e-6 << " mm^2"
         << endl;
    cout << indent_str_next
         << "Peak Dynamic = " << power.readOp.dynamic * coredynp.clockRate
         << " W" << endl;
    cout << indent_str_next << "Subthreshold Leakage = "
         << (long_channel ? power.readOp.longer_channel_leakage
                          : power.readOp.leakage)
         << " W" << endl;
    if (power_gating)
      cout << indent_str_next
           << "Subthreshold Leakage with power gating = "
           << (long_channel ? power.readOp.power_gated_with_long_channel_leakage
                            : power.readOp.power_gated_leakage)
           << " W" << endl;
    cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage
         << " W" << endl;
    cout << indent_str_next
         << "Runtime Dynamic = " << rt_power.readOp.dynamic / coredynp.executionTime
         << " W" << endl;
    cout << indent_str_next
         << "Modeled pc_tag_bits=" << cfg.pc_tag_bits
         << " TT_sets=" << cfg.training_table_sets
         << "x" << cfg.training_table_ways
         << " insert_threshold(policy)=" << cfg.training_insert_threshold
         << " obs_counter_bits=14" << endl;
    cout << endl;
  }

  if (plevel > 2) {
    (void)is_tdp;
  }
}

IFuseU::~IFuseU() {
  if (rlb) {
    delete rlb;
    rlb = 0;
  }
  if (fct) {
    delete fct;
    fct = 0;
  }
  if (training_table) {
    delete training_table;
    training_table = 0;
  }
  if (apt) {
    delete apt;
    apt = 0;
  }
  if (aci) {
    delete aci;
    aci = 0;
  }
  if (tt_plru) {
    delete tt_plru;
    tt_plru = 0;
  }
  if (apt_plru) {
    delete apt_plru;
    apt_plru = 0;
  }
  if (aci_plru) {
    delete aci_plru;
    aci_plru = 0;
  }
}
