#ifndef SCDETECT_APPS_CC_DETECTOR_MATCHRESULT_H_
#define SCDETECT_APPS_CC_DETECTOR_MATCHRESULT_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/timewindow.h>

#include <vector>

namespace Seiscomp {
namespace detect {
namespace detector {

struct MatchResult {
  struct Value {
    Core::TimeSpan lag;
    double coefficient;
  };

  using LocalMaxima = std::vector<Value>;
  LocalMaxima localMaxima;

  // Time window the result is referring to
  Core::TimeWindow timeWindow;
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_MATCHRESULT_H_
