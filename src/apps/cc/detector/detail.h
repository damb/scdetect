#ifndef SCDETECT_APPS_CC_DETECTOR_DETAIL_H_
#define SCDETECT_APPS_CC_DETECTOR_DETAIL_H_

#include <memory>
#include <string>

#include "../filter/crosscorrelation.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace detail {

using ProcessorIdType = std::string;
using WaveformStreamIdType = std::string;

using CrossCorrelation = filter::CrossCorrelation<double>;

}  // namespace detail
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETAIL_H_
