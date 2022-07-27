#ifndef SCDETECT_APPS_CC_DETECTION_H_
#define SCDETECT_APPS_CC_DETECTION_H_

#include <seiscomp/datamodel/arrival.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/pick.h>

#include <string>
#include <vector>

namespace Seiscomp {
namespace detect {

struct Detection {
  using ProcessorId = std::string;
  using WaveformStreamId = std::string;

  struct ArrivalPick {
    DataModel::ArrivalPtr arrival;
    DataModel::PickPtr pick;
  };
  using ArrivalPicks = std::vector<ArrivalPick>;

  struct Pick {
    // The authorative full waveform stream identifier
    WaveformStreamId authorativeWaveformStreamId;
    DataModel::PickCPtr pick;
  };

  // Picks and arrivals which are associated to the detection (i.e. both
  // detected picks and *template picks*)
  ArrivalPicks arrivalPicks;

  DataModel::OriginPtr origin;

  std::string detectorId;

  const std::string &id() const;

  friend bool operator==(const Detection &lhs, const Detection &rhs);
  friend bool operator!=(const Detection &lhs, const Detection &rhs);
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTION_H_
