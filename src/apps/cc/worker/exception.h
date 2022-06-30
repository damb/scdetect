#ifndef SCDETECT_APPS_CC_WORKER_EXCEPTION_H_
#define SCDETECT_APPS_CC_WORKER_EXCEPTION_H_

#include "../exception.h"

namespace Seiscomp {
namespace detect {
namespace worker {

class BaseException : public Exception {
  using Exception::Exception;
  BaseException();
};

}  // namespace worker
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_WORKER_EXCEPTION_H_
