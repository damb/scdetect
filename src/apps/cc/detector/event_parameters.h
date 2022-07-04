#ifndef SCDETECT_APPS_CC_DETECTOR_EVENTPARAMETERS_H_
#define SCDETECT_APPS_CC_DETECTOR_EVENTPARAMETERS_H_

#include <seiscomp/core/datetime.h>

#include <boost/optional/optional.hpp>
#include <string>

namespace Seiscomp {
namespace detect {
namespace detector {

struct Origin {
  // The unique origin identifier (i.e. usually this refers to the unique
  // origin public identifier)
  std::string id;

  // The origin time
  Core::Time time;

  boost::optional<double> depth;
  double latitude{};
  double longitude{};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_EVENTPARAMETERS_H_
