/*****************************************************************************
 * I-Fuse (runtime load fusion) hardware structures for McPAT area/power modeling.
 *
 * Models five on-core tables matching the Scarab runtime I-Fuse design:
 *   RLB  - direct-mapped retired load buffer (retire stage)
 *   TT   - set-associative training table (retire stage)
 *   FCT  - fusion candidate table keyed by LD1 PC (fetch stage)
 *   APT  - set-associative active pair table keyed by LD2 PC (fetch stage)
 *   ACI  - set-associative access-check index keyed by cache block (fetch stage)
 *
 * Storage widths follow Scarab's hardware bit-packing model.  The training
 * observation counter is always 14 bits wide; --ifuse_training_insert_threshold
 * is a promotion policy knob only and does not change TT entry width.
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

  ArrayST* rlb;
  ArrayST* fct;
  ArrayST* training_table;
  ArrayST* apt;
  ArrayST* aci;
  ArrayST* tt_plru;
  ArrayST* apt_plru;
  ArrayST* aci_plru;

  IFuseU(ParseXML* XML_interface, int ithCore_, InputParameter* interface_ip_,
         const CoreDynParam& dyn_p_, bool exist_ = true);
  void computeEnergy(bool is_tdp = true);
  void displayEnergy(uint32_t indent = 0, int plevel = 100, bool is_tdp = true);
  ~IFuseU();
};

#endif /* IFUSE_H_ */
