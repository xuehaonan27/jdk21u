#ifndef SHARE_GC_G1_RMTADDRESS_HPP
#define SHARE_GC_G1_RMTADDRESS_HPP

#include "memory/allStatic.hpp"
#include "utilities/globalDefinitions.hpp"
#include "oops/access.hpp"

class DistributedSharedHandle;

// 0 - 46 bits: Object data address 
// 47 bit: Evacuating
// 53 bit: Dirty (object content modified, should be handled in write barrier)
// 54 bit: Shared distributed oop
// 55 bit: Present in local
// 56 - 62 bits: Hotness
// 63 bit: Is distributed oop
class DistributedOop {
  friend DistributedSharedHandle;
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
  static const uint      max_hotness               = hotness_mask;

  static const int       distributed_shift         = 63;
  static const uintptr_t distributed_mask_in_place = (uintptr_t)1 << distributed_shift;

public:
  explicit DistributedOop(uintptr_t value) : _value(value) {}
  explicit DistributedOop(oop value) : _value(cast_from_oop<uintptr_t>(value)) {}
  DistributedOop() = default; // No default initializer
  // It is critical for performance that this class be trivially
  // destructable, copyable, and assignable.
  ~DistributedOop() = default;
  DistributedOop(const DistributedOop&) = default;
  DistributedOop& operator=(const DistributedOop&) = default;

  uintptr_t value() const { return _value; }

  DistributedOop from_pointer(uintptr_t value) { return DistributedOop(value | distributed_mask_in_place); }

  DistributedOop wrap_a_shared_handle(DistributedSharedHandle* hptr) {
    uintptr_t h = (uintptr_t)(hptr);
    uintptr_t norm_h = h & oop_address_mask_in_place;
    guarantee(norm_h == h, "64 bits pointer should only use bit 0-46");

    // Note that on shared oop, this should only set `distribued_mask_in_place` and `shared_mask_in_place`.
    // Just to indicate that this satisfies predicate `is_distribute_and_shared`.
    uintptr_t v = (uintptr_t)(norm_h | ( distributed_mask_in_place | shared_mask_in_place ));
    DistributedOop ret = DistributedOop(v);
    guarantee(ret.is_distribute_and_shared(), "sanity");
    return ret;
  }

  bool is_evacuating() const {
    return (mask_bits(value(), evacuating_mask_in_place) != 0);
  }

  bool is_dirty() const {
    return (mask_bits(value(), dirty_mask_in_place) != 0);
  }

  bool is_shared() const {
    return (mask_bits(value(), shared_mask_in_place) != 0);
  }

  bool is_distribute_and_shared() const {
    const uintptr_t flag = (distributed_mask_in_place | shared_mask_in_place);
    return (mask_bits(value(), flag) == flag);
  }

  bool is_distribute_and_unique() const {
    const uintptr_t flag = (distributed_mask_in_place | shared_mask_in_place);
    return (mask_bits(value(), flag) == distributed_mask_in_place);
  }

  bool is_present() const {
    return (mask_bits(value(), present_mask_in_place) != 0);
  }

  bool is_distributed() const {
    return (mask_bits(value(), distributed_mask_in_place) != 0);
  }

  uint hotness() const {
    return (mask_bits(value() >> hotness_shift, hotness_mask));
  }

  DistributedOop set_hotness(uint v) const {
    assert((v & ~hotness_mask) == 0, "shouldn't overflow hotness field");
    return DistributedOop((value() & ~hotness_mask_in_place) | ((v & hotness_mask) << hotness_shift));
  }

  DistributedOop incr_hotness() const {
    return hotness() == max_hotness ? DistributedOop(_value) : set_hotness(hotness() + 1);
  }

  inline oop decode_oop() { return cast_to_oop(mask_bits(value() >> oop_address_shift, oop_address_mask)); }
};


// 0 - 46 bits: Object data address 
// 47 bit: Evacuating
// 53 bit: Dirty (object content modified, should be handled in write barrier)
// 54 bit: Shared distributed oop
// 55 bit: Present in local
// 56 - 62 bits: Hotness
// 63 bit: Is distributed oop
class DistributedSharedHandle {
  DistributedOop doop; // This should point to the actual oop
  explicit DistributedSharedHandle(uintptr_t value) : doop(DistributedOop(value)) {}
  // uintptr_t value() const { return doop.value(); }
public:

  DistributedSharedHandle transform_from_unique(DistributedOop doop) {
    guarantee(doop.is_distribute_and_unique(), "sanity");
    DistributedSharedHandle(doop.value() | DistributedOop::shared_mask_in_place);
  }

  inline oop get_wrapped_oop() { 
    return doop.decode_oop();
  }
};


// template <DecoratorSet decorators>
class DistributedOopPostProcessor {
  static oop handle_oop_load(void* addr, oop obj) {
    // TODO: unique / shared is not considered yet.
    DistributedOop dobj = DistributedOop(obj);
    if (dobj.is_present()) {
      // The dobj is present in local memory.
      // Then just record some data like hotness into the addr.
      DistributedOop updated = dobj.incr_hotness();
      oop updated_oop = updated.decode_oop();
      // We do not set INTERNAL_VALUE_IS_OOP here because we only want RawAccessBarrier's handling of memory order
      // Instead of calling oop_store and bump into a recursion.
      RawAccessBarrier<MO_RELEASE>::store((void*)addr, updated_oop);
      return updated_oop;
    } else {
      // The dobj is not present in local memory, then it's remote.
      // This is the slow path, since we must tell runtime to handle this.

      // We need to obtain the thread info to ensure we are in GC thread or Mutator thread ...
      // After the object is swapped back into local memory, 

      // TODO: implement full logic. For now, just return value.
      DistributedOop updated = dobj.incr_hotness();
      oop updated_oop = updated.decode_oop();
      RawAccessBarrier<MO_RELEASE>::store((void*)addr, updated_oop);
      return updated_oop;
    }
  }

  static void handle_oop_store(void* addr, oop obj) {
    
  }
};

#endif // SHARE_GC_G1_RMTADDRESS_HPP
