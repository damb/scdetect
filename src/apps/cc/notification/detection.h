#ifndef SCDETECT_APPS_CC_NOTIFICATION_DETECTION_H_
#define SCDETECT_APPS_CC_NOTIFICATION_DETECTION_H_

#include <seiscomp/core/defs.h>

#include <memory>
#include <string>

#include "../detector/detection.h"
#include "../notification.h"

namespace Seiscomp {
namespace detect {
namespace notification {

DEFINE_SMARTPOINTER(Detection);
// Detection worker notification
class Detection : public WorkerNotification {
 public:
  std::string detectorId;

  std::unique_ptr<detector::Detection> detection;
};

}  // namespace notification
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_NOTIFICATION_DETECTION_H_
