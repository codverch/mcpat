/*****************************************************************************
 * McPAT area, timing, and power model for the I-Fuse tables.
 *****************************************************************************/

#include "ifuse.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>

#include "const.h"

namespace {

const char* kTableNames[IFUSE_NUM_TABLES] = {
    "Fusion Candidate Table (FCT)", "Active Pair Tracker (APT)",
    "Access Check Index (ACI)", "Training Table (TT)",
    "Retired Load Buffer (RLB)"};

int ceil_div8(int bits) { return (bits + 7) / 8; }

void reset_interface_ip(InputParameter* ip) {
  ip->is_cache = false;
  ip->pure_ram = false;
  ip->pure_cam = false;
  ip->specific_tag = 0;
  ip->access_mode = 0;
  ip->obj_func_dyn_energy = 0;
  ip->obj_func_dyn_power = 0;
  ip->obj_func_leak_power = 0;
  ip->obj_func_cycle_t = 1;
  ip->num_rw_ports = 0;
  ip->num_rd_ports = 0;
  ip->num_wr_ports = 0;
  ip->num_se_rd_ports = 0;
  ip->num_search_ports = 0;
  ip->nbanks = 1;
}

/* A set-associative table: CACTI cache mode with an explicit tag width. All
 * ways of a set are read in parallel (access_mode 0, normal). */
ArrayST* make_table(InputParameter* base_ip, const char* name,
                    const ifuse_table_systemcore& t, double clock_rate,
                    const CoreDynParam& dyn) {
  InputParameter ip = *base_ip;
  reset_interface_ip(&ip);
  int line_bytes = std::max(1, ceil_div8(t.data_bits));
  ip.is_cache = true;
  ip.specific_tag = 1;
  ip.tag_w = t.tag_bits;
  ip.line_sz = line_bytes;
  ip.assoc = std::max(1, t.ways);
  ip.cache_sz = std::max(64, t.sets * (int)ip.assoc * line_bytes);
  ip.out_w = line_bytes * 8;
  ip.throughput = 1.0 / clock_rate;
  ip.latency = 1.0 / clock_rate;
  ip.num_rd_ports = std::max(0, t.rd_ports);
  ip.num_wr_ports = std::max(0, t.wr_ports);
  if (ip.num_rd_ports + ip.num_wr_ports == 0) ip.num_rw_ports = 1;
  return new ArrayST(&ip, name, Core_device, dyn.opt_local, dyn.core_ty);
}

/* Replacement state: one row per set, repl_bits_per_set wide. */
ArrayST* make_repl(InputParameter* base_ip, const char* name,
                   const ifuse_table_systemcore& t, double clock_rate,
                   const CoreDynParam& dyn) {
  if (t.repl_bits_per_set <= 0) return NULL;
  InputParameter ip = *base_ip;
  reset_interface_ip(&ip);
  int row_bytes = std::max(1, ceil_div8(t.repl_bits_per_set));
  ip.pure_ram = true;
  ip.line_sz = row_bytes;
  ip.assoc = 1;
  ip.cache_sz = std::max(64, t.sets * row_bytes);
  ip.out_w = row_bytes * 8;
  ip.throughput = 1.0 / clock_rate;
  ip.latency = 1.0 / clock_rate;
  ip.num_rd_ports = 1;
  ip.num_wr_ports = 1;
  return new ArrayST(&ip, name, Core_device, dyn.opt_local, dyn.core_ty);
}

void set_activity(ArrayST* arr, double reads, double writes) {
  if (!arr) return;
  arr->stats_t.readAc.access = reads;
  arr->stats_t.writeAc.access = writes;
  arr->power_t.reset();
  arr->power_t.readOp.dynamic =
      arr->local_result.power.readOp.dynamic * reads +
      arr->local_result.power.writeOp.dynamic * writes;
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
      exist(exist_) {
  for (int t = 0; t < IFUSE_NUM_TABLES; t++) {
    table[t] = NULL;
    repl[t] = NULL;
  }
  if (!exist) return;

  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;
  if (cfg.enabled <= 0 && XML->sys.core[ithCore].number_of_ifuse <= 0) {
    exist = false;
    return;
  }

  const double clock_rate = coredynp.clockRate;
  for (int t = 0; t < IFUSE_NUM_TABLES; t++) {
    table[t] = make_table(&interface_ip, kTableNames[t], cfg.table[t],
                          clock_rate, coredynp);
    area.set_area(area.get_area() + table[t]->local_result.area);
    repl[t] = make_repl(&interface_ip, kTableNames[t], cfg.table[t],
                        clock_rate, coredynp);
    if (repl[t]) area.set_area(area.get_area() + repl[t]->local_result.area);
  }
}

void IFuseU::computeEnergy(bool is_tdp) {
  if (!exist) return;

  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;
  powerDef p;

  for (int t = 0; t < IFUSE_NUM_TABLES; t++) {
    double reads, writes;
    if (is_tdp) {
      /* Peak: every port busy every cycle. */
      reads = std::max(1, cfg.table[t].rd_ports);
      writes = std::max(0, cfg.table[t].wr_ports);
    } else {
      reads = cfg.table[t].read_accesses;
      writes = cfg.table[t].write_accesses;
    }
    set_activity(table[t], reads, writes);
    /* Every hit or fill updates the replacement state. */
    set_activity(repl[t], reads, reads + writes);
    if (table[t]) p = p + table[t]->power;
    if (repl[t]) p = p + repl[t]->power;
  }

  if (is_tdp)
    power = p;
  else
    rt_power = p;
}

void IFuseU::displayEnergy(uint32_t indent, int plevel, bool is_tdp) {
  if (!exist || !is_tdp) return;
  std::string ind(indent, ' ');
  std::string ind2(indent + 2, ' ');
  std::string ind4(indent + 4, ' ');
  bool long_channel = XML->sys.longer_channel_device;
  const ifuse_systemcore& cfg = XML->sys.core[ithCore].ifuse;
  const double cycle_ps = 1e12 / coredynp.clockRate;

  cout << ind << "I-Fuse:" << endl;
  cout << ind2 << "Area = " << area.get_area() * 1e-6 << " mm^2" << endl;
  cout << ind2 << "Peak Dynamic = " << power.readOp.dynamic * coredynp.clockRate
       << " W" << endl;
  cout << ind2 << "Subthreshold Leakage = "
       << (long_channel ? power.readOp.longer_channel_leakage
                        : power.readOp.leakage)
       << " W" << endl;
  cout << ind2 << "Gate Leakage = " << power.readOp.gate_leakage << " W"
       << endl;
  cout << ind2 << "Runtime Dynamic = "
       << rt_power.readOp.dynamic / coredynp.executionTime << " W" << endl;
  cout << ind2 << "Clock period = " << cycle_ps << " ps" << endl;

  for (int t = 0; t < IFUSE_NUM_TABLES; t++) {
    if (!table[t]) continue;
    const ifuse_table_systemcore& tc = cfg.table[t];
    const uca_org_t& r = table[t]->local_result;
    double area_um2 = r.area + (repl[t] ? repl[t]->local_result.area : 0);
    double leak = (long_channel ? r.power.readOp.longer_channel_leakage
                                : r.power.readOp.leakage) +
                  r.power.readOp.gate_leakage;
    if (repl[t])
      leak += (long_channel
                   ? repl[t]->local_result.power.readOp.longer_channel_leakage
                   : repl[t]->local_result.power.readOp.leakage) +
              repl[t]->local_result.power.readOp.gate_leakage;
    double acc_ps = r.access_time * 1e12;
    cout << ind2 << kTableNames[t] << ":" << endl;
    cout << ind4 << "Organization = " << tc.sets << " sets x " << tc.ways
         << " ways, tag " << tc.tag_bits << " b + data " << tc.data_bits
         << " b, " << tc.rd_ports << "R/" << tc.wr_ports << "W" << endl;
    cout << ind4 << "Storage = "
         << tc.sets * tc.ways * (tc.tag_bits + tc.data_bits) +
                tc.sets * tc.repl_bits_per_set
         << " bits" << endl;
    cout << ind4 << "Area = " << area_um2 * 1e-6 << " mm^2" << endl;
    cout << ind4 << "Access time = " << acc_ps << " ps ("
         << std::fixed << std::setprecision(2) << acc_ps / cycle_ps
         << " cycles)" << std::defaultfloat << std::setprecision(6) << endl;
    cout << ind4 << "Cycle time = " << r.cycle_time * 1e12 << " ps" << endl;
    cout << ind4 << "Read energy = " << r.power.readOp.dynamic * 1e12
         << " pJ" << endl;
    cout << ind4 << "Write energy = " << r.power.writeOp.dynamic * 1e12
         << " pJ" << endl;
    cout << ind4 << "Leakage (sub+gate) = " << leak * 1e3 << " mW" << endl;
    cout << ind4 << "Reads = " << tc.read_accesses
         << ", Writes = " << tc.write_accesses << endl;
  }
  cout << endl;
  (void)plevel;
}

IFuseU::~IFuseU() {
  for (int t = 0; t < IFUSE_NUM_TABLES; t++) {
    if (table[t]) delete table[t];
    if (repl[t]) delete repl[t];
    table[t] = NULL;
    repl[t] = NULL;
  }
}
