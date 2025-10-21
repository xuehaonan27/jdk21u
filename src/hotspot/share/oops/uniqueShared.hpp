#ifndef SHARE_OOPS_UNIQUE_SHARED_HPP
#define SHARE_OOPS_UNIQUE_SHARED_HPP

#include "metaprogramming/primitiveConversions.hpp"
#include "utilities/globalDefinitions.hpp"
#include "oops/oop.hpp"

class SpecialOopRef {
 private:
  uintptr_t _value;

 public:
  explicit SpecialOopRef(uintptr_t value) : _value(value) {}

  SpecialOopRef() = default; // Doesn't initialize _value.

  // It is critical for performance that this class be trivially
  // destructable, copyable, and assignable.
  ~SpecialOopRef() = default;
  SpecialOopRef(const SpecialOopRef&) = default;
  SpecialOopRef& operator=(const SpecialOopRef&) = default;

  static SpecialOopRef from_pointer(void* ptr) {
    return SpecialOopRef((uintptr_t)ptr);
  }

  static SpecialOopRef from_oop(oop obj) {
    return SpecialOopRef((uintptr_t)cast_from_oop(obj));
  }

  oop as_oop() {
    return cast_to_oop(address());
  }

  bool operator==(const SpecialOopRef& other) const {
    return _value == other._value;
  }
  bool operator!=(const SpecialOopRef& other) const {
    return !operator==(other);
  }

  uintptr_t value() const { return _value; }

  // Constants
  static const int address_bits = 47;
  static const int address_shift = 0;
  static const uintptr_t address_mask = right_n_bits(address_bits);
  static const uintptr_t address_mask_in_place = address_mask << address_shift;

  static const uintptr_t evacuating_mask_in_place = 1 << 47;
  static const uintptr_t dirty_mask_in_place      = 1 << 53;
  static const uintptr_t shared_mask_in_place     = 1 << 54;
  static const uintptr_t present_mask_in_place    = 1 << 55;

  static const int hotness_bits = 7;
  static const int hotness_shift = 56;
  static const uintptr_t hotness_mask = right_n_bits(hotness_bits);
  static const uintptr_t hotness_mask_in_place = hotness_mask << hotness_shift;
  static const uint max_hotness = hotness_mask;

  static const uintptr_t special_ref_mask_in_place = 1 << 63;

  // oop address (lower 47 bits)
  SpecialOopRef address() const {
    return SpecialOopRef(value() & address_mask_in_place);
  }

  // hotness
  uint hotness() const {
    return SpecialOopRef(value() & hotness_mask_in_place);
  }
  SpecialOopRef incr_hotness() const {
    return hotness() == max_hotness ? SpecialOopRef(_value) : set_hotness(hotness() + 1);
  }
  SpecialOopRef decr_hotness() const {
    return hotness() == 0 ? SpecialOopRef(_value) : set_hotness(hotness() - 1);
  }

  // evacuating
  bool is_evacuating() const {
    return ((value() & evacuating_mask_in_place) != 0);
  }
  SpecialOopRef set_evacuating() const {
    return SpecialOopRef(value() | evacuating_mask_in_place);
  }
  SpecialOopRef clear_evacuating() const {
    return SpecialOopRef(value() & ~evacuating_mask_in_place);
  }

  // dirty
  bool is_dirty() const {
    return ((value() & dirty_mask_in_place) != 0);
  }
  SpecialOopRef set_dirty() const {
    return SpecialOopRef(value() | dirty_mask_in_place);
  }
  SpecialOopRef clear_dirty() const {
    return SpecialOopRef(value() & ~dirty_mask_in_place);
  }

  // shared
  bool is_shared() const {
    return ((value() & shared_mask_in_place) != 0);
  }
  SpecialOopRef set_shared() const {
    return SpecialOopRef(value() | shared_mask_in_place);
  }
  SpecialOopRef clear_shared() const {
    return SpecialOopRef(value() & ~shared_mask_in_place);
  }

  // present
  bool is_present() const {
    return ((value() & present_mask_in_place) != 0);
  }
  SpecialOopRef set_present() const {
    return SpecialOopRef(value() | present_mask_in_place);
  }
  SpecialOopRef clear_present() const {
    return SpecialOopRef(value() & ~present_mask_in_place);
  }

  // is_special_ref
  bool is_special_ref() const {
    return ((value() & special_ref_mask_in_place) != 0);
  }
  SpecialOopRef set_special_ref() const {
    return SpecialOopRef(value() | special_ref_mask_in_place);
  }
  SpecialOopRef clear_special_ref() const {
    return SpecialOopRef(value() & ~special_ref_mask_in_place);
  }

  // check ref
  bool is_shared_ref() const {
    return (is_special_ref() && is_shared()); 
  }
  bool is_unique_ref() const {
    return (is_special_ref() && (!is_shared()));
  }
};

#endif // SHARE_OOPS_UNIQUE_SHARED_HPP
