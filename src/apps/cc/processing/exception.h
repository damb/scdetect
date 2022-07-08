#ifndef SCDETECT_APPS_CC_PROCESSING_EXCEPTION_H_
#define SCDETECT_APPS_CC_PROCESSING_EXCEPTION_H_

#include "../exception.h"

namespace Seiscomp {
namespace detect {
namespace processing {

// Base class for all processing related exceptions
class BaseException : public Exception {
 public:
  using Exception::Exception;
  BaseException();
};

}  // namespace processing
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_PROCESSING_EXCEPTION_H_
