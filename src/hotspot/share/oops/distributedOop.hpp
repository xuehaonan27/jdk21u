#ifndef SHARE_GC_G1_RMTADDRESS_HPP
#define SHARE_GC_G1_RMTADDRESS_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"

// 0 - 46 bits: Object data address 
// 47 bit: Evacuating
// 53 bit: Dirty (object content modified)
// 54 bit: Shared distributed oop
// 55 bit: Present in local
// 56 - 62 bits: Hotness
// 63 bit: Is distributed oop
class DistributedOop {
private:
  uintptr_t _value;

  static const int       oop_address_bits          = 47;
  static const int       oop_address_shift         = 0;
  static const uintptr_t oop_address_mask          = right_n_bits(oop_address_bits);
  static const uintptr_t oop_address_mask_in_place = oop_address_mask << oop_address_shift;

  static const int       evacuating_shift          = 47;
  static const uintptr_t evacuating_mask_in_place  = (uintptr_t)1 << evacuating_shift;

  static const int       dirty_shift               = 53;
  static const uintptr_t dirty_mask_in_place       = (uintptr_t)1 << dirty_shift;

  static const int       shared_shift              = 54;
  static const uintptr_t shared_mask_in_place      = (uintptr_t)1 << shared_shift;

  static const int       present_shift             = 55;
  static const uintptr_t present_mask_in_place     = (uintptr_t)1 << present_shift;

  static const int       hotness_bits              = 6;
  static const int       hotness_shift             = 56;
  static const uintptr_t hotness_mask              = right_n_bits(hotness_bits);
  static const uintptr_t hotness_mask_in_place     = hotness_mask << hotness_shift;

  static const int       distributed_shift         = 63;
  static const uintptr_t distributed_mask_in_place = (uintptr_t)1 << distributed_shift;

public:
  explicit DistributedOop(uintptr_t value) : _value(value) {}
  DistributedOop() = default; // Doesn't initialize _value.
  // It is critical for performance that this class be trivially
  // destructable, copyable, and assignable.
  ~DistributedOop() = default;
  DistributedOop(const DistributedOop&) = default;
  DistributedOop& operator=(const DistributedOop&) = default;

  uintptr_t value() const { return _value; }

  bool is_evacuating() const {
    return (mask_bits(value(), evacuating_mask_in_place) != 0);
  }

  bool is_dirty() const {
    return (mask_bits(value(), dirty_mask_in_place) != 0);
  }

  bool is_shared() const {
    return (mask_bits(value(), shared_mask_in_place) != 0);
  }

  bool is_present() const {
    return (mask_bits(value(), present_mask_in_place));
  }

  bool is_distributed() const {
    return (mask_bits());
  }

  uint hotness() const {
    return (mask_bits(value() >> hotness_shift, hotness_mask));
  }

  inline oop decode_oop() { return mask_bits(value() >> oop_address_shift, oop_address_mask); }
};

#endif // SHARE_GC_G1_RMTADDRESS_HPP
