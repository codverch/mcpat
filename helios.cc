/*****************************************************************************
 * Helios load/store fusion power model for McPAT.
 *
 * Models the Scarab Helios structures:
 *  - local/global fusion predictors (set-associative)
 *  - selector table
 *  - load/store head tables
 *  - register tracking table
 *  - active fusion interval ring
 *  - load/store unbounded commit history (UCH)
 *****************************************************************************/

#include "helios.h"

#include <cmath>
#include <iostream>
#include <string>

using namespace std;

static ArrayST *make_helios_array(InputParameter &ip, const char *name, int entry_bytes, int num_entries,
                                  int assoc, bool is_cache, int rd_ports, int wr_ports, double clockRate,
                                  CoreDynParam &coredynp) {
  ip.assoc = assoc;
  ip.pure_cam = false;
  ip.pure_ram = !is_cache;
  ip.is_cache = is_cache;
  ip.line_sz = entry_bytes;
  ip.cache_sz = (uint64_t)entry_bytes * (uint64_t)num_entries;
  ip.nbanks = 1;
  ip.out_w = entry_bytes * 8;
  ip.access_mode = 2;
  ip.throughput = 1.0 / clockRate;
  ip.latency = 1.0 / clockRate;
  ip.obj_func_dyn_energy = 0;
  ip.obj_func_dyn_power = 0;
  ip.obj_func_leak_power = 0;
  ip.obj_func_cycle_t = 1;
  ip.num_rw_ports = 0;
  ip.num_rd_ports = rd_ports;
  ip.num_wr_ports = wr_ports;
  ip.num_se_rd_ports = 0;
  return new ArrayST(&ip, name, Core_device, coredynp.opt_local, coredynp.core_ty);
}

static void drive_array(ArrayST *array, double read_accesses, double write_accesses, bool is_tdp) {
  if (!array) return;
  array->stats_t.readAc.access = read_accesses;
  array->stats_t.writeAc.access = write_accesses;
  if (is_tdp) {
    array->tdp_stats = array->stats_t;
  } else {
    array->rtp_stats = array->stats_t;
  }
  array->power_t.reset();
  array->power_t.readOp.dynamic += array->local_result.power.readOp.dynamic * array->stats_t.readAc.access +
                                   array->stats_t.writeAc.access * array->local_result.power.writeOp.dynamic;
}

static void finalize_array(ArrayST *array, bool is_tdp, CoreDynParam &coredynp, powerDef &aggregate_power,
                           powerDef &aggregate_rt_power) {
  if (!array) return;
  if (is_tdp) {
    array->power = array->power_t + array->local_result.power * pppm_lkg;
    aggregate_power = aggregate_power + array->power;
  } else {
    array->rt_power = array->power_t + array->local_result.power * pppm_lkg;
    aggregate_rt_power = aggregate_rt_power + array->rt_power;
  }
}

HeliosUnit::HeliosUnit(ParseXML *XML_interface, int ithCore_, InputParameter *interface_ip_,
                       const CoreDynParam &dyn_p_, bool exist_)
    : XML(XML_interface),
      ithCore(ithCore_),
      interface_ip(*interface_ip_),
      coredynp(dyn_p_),
      clockRate(coredynp.clockRate),
      executionTime(coredynp.executionTime),
      exist(exist_ && XML->sys.core[ithCore].helios.enabled),
      local_predictor(0),
      global_predictor(0),
      selector_table(0),
      load_head_table(0),
      store_head_table(0),
      reg_track_table(0),
      fusion_ring(0),
      load_uch(0),
      store_uch(0) {
  if (!exist) return;

  helios_systemcore &cfg = XML->sys.core[ithCore].helios;
  const int fp_entry_bytes = cfg.fp_entry_bytes;
  const int fp_sets = cfg.fp_sets;
  const int fp_ways = cfg.fp_ways;
  const int selector_entry_bytes = cfg.selector_entry_bytes;
  const int selector_entries = cfg.selector_entries;
  const int head_entry_bytes = cfg.head_entry_bytes;
  const int load_head_entries = cfg.load_head_entries;
  const int store_head_entries = cfg.store_head_entries;
  const int reg_track_entry_bytes = cfg.reg_track_entry_bytes;
  const int reg_track_entries = cfg.reg_track_entries;
  const int fusion_ring_entry_bytes = cfg.fusion_ring_entry_bytes;
  const int fusion_ring_entries = cfg.fusion_ring_entries;
  const int uch_entry_bytes = cfg.uch_entry_bytes;
  const int uch_load_entries = cfg.uch_load_entries;
  const int uch_store_entries = cfg.uch_store_entries;

  local_predictor =
      make_helios_array(interface_ip, "Helios Local Fusion Predictor", fp_entry_bytes, fp_sets * fp_ways, fp_ways,
                        true, 1, 1, clockRate, coredynp);
  global_predictor =
      make_helios_array(interface_ip, "Helios Global Fusion Predictor", fp_entry_bytes, fp_sets * fp_ways, fp_ways,
                        true, 1, 1, clockRate, coredynp);
  selector_table = make_helios_array(interface_ip, "Helios Selector Table", selector_entry_bytes, selector_entries, 1,
                                     false, 1, 1, clockRate, coredynp);
  load_head_table = make_helios_array(interface_ip, "Helios Load Head Table", head_entry_bytes, load_head_entries, 1,
                                      false, 1, 1, clockRate, coredynp);
  store_head_table = make_helios_array(interface_ip, "Helios Store Head Table", head_entry_bytes, store_head_entries, 1,
                                       false, 1, 1, clockRate, coredynp);
  reg_track_table = make_helios_array(interface_ip, "Helios Register Track Table", reg_track_entry_bytes,
                                      reg_track_entries, 1, false, 1, 1, clockRate, coredynp);
  fusion_ring = make_helios_array(interface_ip, "Helios Active Fusion Ring", fusion_ring_entry_bytes,
                                  fusion_ring_entries, 1, false, 1, 1, clockRate, coredynp);
  load_uch = make_helios_array(interface_ip, "Helios Load UCH", uch_entry_bytes, uch_load_entries, 1, false, 1, 1,
                               clockRate, coredynp);
  store_uch = make_helios_array(interface_ip, "Helios Store UCH", uch_entry_bytes, uch_store_entries, 1, false, 1, 1,
                                clockRate, coredynp);

  area.set_area(area.get_area() + local_predictor->local_result.area);
  area.set_area(area.get_area() + global_predictor->local_result.area);
  area.set_area(area.get_area() + selector_table->local_result.area);
  area.set_area(area.get_area() + load_head_table->local_result.area);
  area.set_area(area.get_area() + store_head_table->local_result.area);
  area.set_area(area.get_area() + reg_track_table->local_result.area);
  area.set_area(area.get_area() + fusion_ring->local_result.area);
  area.set_area(area.get_area() + load_uch->local_result.area);
  area.set_area(area.get_area() + store_uch->local_result.area);
}

void HeliosUnit::computeEnergy(bool is_tdp) {
  if (!exist) return;

  helios_systemcore &stats = XML->sys.core[ithCore].helios;
  double pred_reads = 0.0;
  double pred_writes = 0.0;
  double head_reads = 0.0;
  double head_writes = 0.0;
  double reg_reads = 0.0;
  double reg_writes = 0.0;
  double uch_reads = 0.0;
  double uch_writes = 0.0;
  double ring_reads = 0.0;
  double ring_writes = 0.0;

  if (is_tdp) {
    pred_reads = coredynp.LSU_duty_cycle;
    pred_writes = 0.1 * coredynp.LSU_duty_cycle;
    head_reads = coredynp.LSU_duty_cycle;
    head_writes = 0.5 * coredynp.LSU_duty_cycle;
    reg_reads = 0.25 * coredynp.LSU_duty_cycle;
    reg_writes = coredynp.LSU_duty_cycle;
    uch_reads = 0.5 * coredynp.LSU_duty_cycle;
    uch_writes = 0.25 * coredynp.LSU_duty_cycle;
    ring_reads = 0.1 * coredynp.LSU_duty_cycle;
    ring_writes = 0.05 * coredynp.LSU_duty_cycle;
  } else {
    pred_reads = stats.predictor_read_accesses;
    pred_writes = stats.predictor_write_accesses;
    head_reads = stats.head_table_read_accesses;
    head_writes = stats.head_table_write_accesses;
    reg_reads = stats.reg_track_read_accesses;
    reg_writes = stats.reg_track_write_accesses;
    uch_reads = stats.uch_read_accesses;
    uch_writes = stats.uch_write_accesses;
    ring_reads = stats.fusion_ring_read_accesses;
    ring_writes = stats.fusion_ring_write_accesses;
  }

  drive_array(local_predictor, pred_reads, pred_writes, is_tdp);
  drive_array(global_predictor, pred_reads, pred_writes, is_tdp);
  drive_array(selector_table, pred_reads, pred_writes, is_tdp);
  drive_array(load_head_table, head_reads, head_writes, is_tdp);
  drive_array(store_head_table, head_reads, head_writes, is_tdp);
  drive_array(reg_track_table, reg_reads, reg_writes, is_tdp);
  drive_array(fusion_ring, ring_reads, ring_writes, is_tdp);
  drive_array(load_uch, uch_reads, uch_writes, is_tdp);
  drive_array(store_uch, uch_reads, uch_writes, is_tdp);

  power.reset();
  rt_power.reset();
  finalize_array(local_predictor, is_tdp, coredynp, power, rt_power);
  finalize_array(global_predictor, is_tdp, coredynp, power, rt_power);
  finalize_array(selector_table, is_tdp, coredynp, power, rt_power);
  finalize_array(load_head_table, is_tdp, coredynp, power, rt_power);
  finalize_array(store_head_table, is_tdp, coredynp, power, rt_power);
  finalize_array(reg_track_table, is_tdp, coredynp, power, rt_power);
  finalize_array(fusion_ring, is_tdp, coredynp, power, rt_power);
  finalize_array(load_uch, is_tdp, coredynp, power, rt_power);
  finalize_array(store_uch, is_tdp, coredynp, power, rt_power);
}

void HeliosUnit::displayEnergy(uint32_t indent, int plevel, bool is_tdp) {
  if (!exist) return;
  string indent_str(indent, ' ');
  string indent_str_next(indent + 2, ' ');
  bool long_channel = XML->sys.longer_channel_device;
  bool power_gating = XML->sys.power_gating;
  powerDef *target_power = is_tdp ? &power : &rt_power;
  double runtime_divisor = is_tdp ? 1.0 : executionTime;

  cout << indent_str << "Helios Fusion Unit:" << endl;
  cout << indent_str_next << "Area = " << area.get_area() * 1e-6 << " mm^2" << endl;
  cout << indent_str_next << "Peak Dynamic = " << power.readOp.dynamic * clockRate << " W" << endl;
  cout << indent_str_next << "Subthreshold Leakage = "
       << (long_channel ? power.readOp.longer_channel_leakage : power.readOp.leakage) << " W" << endl;
  if (power_gating)
    cout << indent_str_next << "Subthreshold Leakage with power gating = "
         << (long_channel ? power.readOp.power_gated_with_long_channel_leakage : power.readOp.power_gated_leakage)
         << " W" << endl;
  cout << indent_str_next << "Gate Leakage = " << power.readOp.gate_leakage << " W" << endl;
  cout << indent_str_next << "Runtime Dynamic = " << target_power->readOp.dynamic / runtime_divisor << " W" << endl;
  cout << endl;

  if (plevel <= 2) return;

  ArrayST *arrays[] = {local_predictor,   global_predictor, selector_table, load_head_table, store_head_table,
                       reg_track_table,   fusion_ring,      load_uch,       store_uch};
  const char *names[] = {"Local Fusion Predictor", "Global Fusion Predictor", "Selector Table", "Load Head Table",
                         "Store Head Table",       "Register Track Table",  "Fusion Ring",    "Load UCH",
                         "Store UCH"};
  for (int i = 0; i < 9; ++i) {
    ArrayST *array = arrays[i];
    if (!array) continue;
    cout << indent_str_next << names[i] << ":" << endl;
    cout << indent_str_next << "  Area = " << array->area.get_area() * 1e-6 << " mm^2" << endl;
    cout << indent_str_next << "  Runtime Dynamic = "
         << (is_tdp ? array->power.readOp.dynamic * clockRate : array->rt_power.readOp.dynamic / executionTime) << " W"
         << endl;
  }
}

HeliosUnit::~HeliosUnit() {
  delete local_predictor;
  delete global_predictor;
  delete selector_table;
  delete load_head_table;
  delete store_head_table;
  delete reg_track_table;
  delete fusion_ring;
  delete load_uch;
  delete store_uch;
}
