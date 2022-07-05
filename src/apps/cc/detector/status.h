#ifndef SCDETECT_APPS_CC_DETECTOR_STATUS_H_
#define SCDETECT_APPS_CC_DETECTOR_STATUS_H_

namespace Seiscomp {
namespace detect {
namespace detector {

enum class Status {
  kWaitingForData = 0,
  // Associated value is the last status
  kTerminated,
  // No associated value yet (error code?)
  kError,
  // Indicates saturated/clipped data
  kDataClipped,
};

}
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_STATUS_H_
