#pragma once
#include <cstddef>
namespace vc {
class OccupancyStats {
public:
  void sample(size_t occ) { if (occ > hi_) hi_ = occ; }
  size_t high_water() const { return hi_; }
  void reset() { hi_ = 0; }
private:
  size_t hi_ = 0;
};
}
