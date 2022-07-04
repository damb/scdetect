#ifndef SCDETECT_APPS_CC_DETECTOR_INVENTORY_H_
#define SCDETECT_APPS_CC_DETECTOR_INVENTORY_H_

#include <string>

namespace Seiscomp {
namespace detect {
namespace detector {

struct Station {
  // The unique station identifier (i.e. usually refers to the unique station
  // public identifier)
  std::string id;
};

struct SensorLocation {
  Station station;

  // The unique sensor location identifier
  std::string id;

  double latitude{};
  double longitude{};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_INVENTORY_H_
