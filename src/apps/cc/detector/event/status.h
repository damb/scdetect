#ifndef SCDETECT_APPS_CC_DETECTOR_EVENT_STATUS_H_
#define SCDETECT_APPS_CC_DETECTOR_EVENT_STATUS_H_

#include "../detail.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace event {

struct Status {
  enum class Type {
    kFinishedProcessing,
  };

  detail::ProcessorIdType detectorId;
  detail::ProcessorIdType templateProcessorId;

  Type type;
};

}  // namespace event
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EVENT_STATUS_H_
