#include "detection.h"

namespace Seiscomp {
namespace detect {

const std::string &Detection::id() const { return origin->publicID(); }

bool operator==(const Detection &lhs, const Detection &rhs) {
  return lhs.id() == rhs.id();
}

bool operator!=(const Detection &lhs, const Detection &rhs) {
  return !(lhs == rhs);
}

}  // namespace detect
}  // namespace Seiscomp
