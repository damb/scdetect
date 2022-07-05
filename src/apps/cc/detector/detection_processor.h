#ifndef SCDETECT_APPS_CC_DETECTOR_DETECTIONPROCESSOR_H_
#define SCDETECT_APPS_CC_DETECTOR_DETECTIONPROCESSOR_H_

#include <seiscomp/core/datetime.h>

#include <memory>
#include <unordered_map>

#include "../config/detector.h"
#include "detail.h"
#include "detection.h"
#include "detection_candidate.h"
#include "event_parameters.h"
#include "inventory.h"

namespace Seiscomp {
namespace detect {
namespace detector {

class Detector;

// Emerges detections based on a candidate and performs a so-called alignment
// correction.
struct DetectionProcessor {
  struct TemplateProcessorInfo {
    // The sensor location the template processor is referring to
    SensorLocation sensorLocation;
    // The template waveform reference time (i.e. the reference time template
    // waveform was generated based on)
    Core::Time templateWaveformReferenceTime;
  };

  using TemplateProcessorInfos =
      std::unordered_map<detail::ProcessorIdType, TemplateProcessorInfo>;
  TemplateProcessorInfos templateProcessorInfos;

  // The detector related publish configuration
  config::PublishConfig publishConfig;

  // The template origin
  Origin templateOrigin;

  // The detection time correction
  Core::TimeSpan timeCorrection;

  // Creates a `Detection` from `candidate`
  std::unique_ptr<Detection> operator()(
      const Detector* detector, const DetectionCandidate& candidate) const;
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETECTIONPROCESSOR_H_
