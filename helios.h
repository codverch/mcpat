/*****************************************************************************
 * Helios load/store fusion power model for McPAT.
 *****************************************************************************/

#ifndef HELIOS_H_
#define HELIOS_H_

#include "XML_Parse.h"
#include "array.h"
#include "basic_components.h"

class HeliosUnit : public Component {
 public:
  ParseXML *XML;
  int ithCore;
  InputParameter interface_ip;
  CoreDynParam coredynp;
  double clockRate;
  double executionTime;
  bool exist;

  ArrayST *local_predictor;
  ArrayST *global_predictor;
  ArrayST *selector_table;
  ArrayST *load_head_table;
  ArrayST *store_head_table;
  ArrayST *reg_track_table;
  ArrayST *fusion_ring;
  ArrayST *load_uch;
  ArrayST *store_uch;

  HeliosUnit(ParseXML *XML_interface, int ithCore_, InputParameter *interface_ip_,
             const CoreDynParam &dyn_p_, bool exist_ = true);
  void computeEnergy(bool is_tdp = true);
  void displayEnergy(uint32_t indent = 0, int plevel = 100, bool is_tdp = true);
  ~HeliosUnit();
};

#endif /* HELIOS_H_ */
