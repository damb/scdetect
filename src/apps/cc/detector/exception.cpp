#include "exception.h"

namespace Seiscomp {
namespace detect {
namespace detector {

BaseException::BaseException()
    : processing::Processor::BaseException{"base detector exception"} {}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
