#include "exception.h"

namespace Seiscomp {
namespace detect {
namespace processing {

BaseException::BaseException() : Exception{"base processing exception"} {}

}  // namespace processing
}  // namespace detect
}  // namespace Seiscomp
