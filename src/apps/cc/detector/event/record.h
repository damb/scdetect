#ifndef SCDETECT_APPS_CC_DETECTOR_EVENT_RECORD_H_
#define SCDETECT_APPS_CC_DETECTOR_EVENT_RECORD_H_

#include <seiscomp/core/record.h>

namespace Seiscomp {
namespace detect {
namespace detector {
namespace event {

struct Record {
  RecordCPtr record;
};

}  // namespace event
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EVENT_RECORD_H_
