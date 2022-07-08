#ifndef SCDETECT_APPS_CC_DETECTOR_EXCEPTION_H_
#define SCDETECT_APPS_CC_DETECTOR_EXCEPTION_H_

#include "../processing/processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

class BaseException : public processing::Processor::BaseException {
 public:
  using processing::Processor::BaseException::BaseException;
  BaseException();
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EXCEPTION_H_
