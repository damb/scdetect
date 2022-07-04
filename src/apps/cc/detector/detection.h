#ifndef SCDETECT_APPS_CC_DETECTOR_DETECTION_H_
#define SCDETECT_APPS_CC_DETECTOR_DETECTION_H_

#include <seiscomp/core/datetime.h>

#include <boost/optional/optional.hpp>
#include <unordered_map>

#include "../config/detector.h"
#include "arrival.h"
#include "detail.h"
#include "inventory.h"

namespace Seiscomp {
namespace detect {
namespace detector {

struct Detection {
  struct ContributionInfo {
    Arrival arrival;
    SensorLocation sensorLocation;

    Core::Time templateWaveformStartTime;
    Core::Time templateWaveformEndTime;

    // The template waveform referece time (i.e. the reference time used for
    // template waveform creation)
    Core::Time templateWaveformReferenceTime;
  };

  using ContributionInfos =
      std::unordered_map<detail::ProcessorIdType, ContributionInfo>;
  ContributionInfos contributionInfos;

  config::PublishConfig publishConfig;

  // The detection origin time
  Core::Time originTime;

  // The detection score. In case of a single stream detector setup this
  // corresponds to the cross-correlation coefficient.
  double score{};

  double latitude{};
  double longitude{};
  boost::optional<double> depth;

  int numStationsAssociated{};
  int numStationsUsed{};
  int numChannelsAssociated{};
  int numChannelsUsed{};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETECTION_H_
