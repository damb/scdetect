#ifndef SCDETECT_APPS_CC_DETECTOR_EVENT_LINK_H_
#define SCDETECT_APPS_CC_DETECTOR_EVENT_LINK_H_

#include "../detail.h"
#include "../match_result.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace event {

struct Link {
  detail::ProcessorIdType detectorId;
  detail::ProcessorIdType templateProcessorId;

  MatchResult matchResult;
};

}  // namespace event
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EVENT_LINK_H_
