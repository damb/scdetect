#include "exception.h"

namespace Seiscomp {
namespace detect {
namespace worker {

BaseException::BaseException() : Exception{"base worker exception"} {}

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp
