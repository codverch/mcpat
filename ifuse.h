/*****************************************************************************
 * I-Fuse (correlated load micro-op fusion) structures for McPAT.
 *
 * Models the five set-associative tables of Scarab hpca2027-revision-ifuse:
 *   FCT - Fusion Candidate Table, indexed by LD1 PC (decode)
 *   APT - Active Pair Tracker, indexed by LD2 PC (decode, rename)
 *   ACI - Access Check Index, indexed by block address (execute)
 *   TT  - Training Table, indexed by LD1 PC (retire)
 *   RLB - Retired Load Buffer, indexed by block address (retire)
 *
 * Each table is a CACTI cache-mode array with a tag array and a data array,
 * plus an optional replacement-state RAM (tree-PLRU bits per set).
 *****************************************************************************/

#ifndef IFUSE_H_
#define IFUSE_H_

#include "array.h"
#include "basic_components.h"
#include "component.h"
#include "XML_Parse.h"

class IFuseU : public Component {
 public:
  ParseXML* XML;
  int ithCore;
  InputParameter interface_ip;
  CoreDynParam coredynp;
  bool exist;

  ArrayST* table[IFUSE_NUM_TABLES];
  ArrayST* repl[IFUSE_NUM_TABLES];

  IFuseU(ParseXML* XML_interface, int ithCore_, InputParameter* interface_ip_,
         const CoreDynParam& dyn_p_, bool exist_ = true);
  void computeEnergy(bool is_tdp = true);
  void displayEnergy(uint32_t indent = 0, int plevel = 100, bool is_tdp = true);
  ~IFuseU();
};

#endif /* IFUSE_H_ */
