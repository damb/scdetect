#ifndef SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATEPROCESSOR_H_
#define SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATEPROCESSOR_H_

#include <seiscomp/core/datetime.h>

#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "detail.h"
#include "detection.h"
#include "detection_candidate.h"
#include "detection_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

class Detector;

// (Post)processor for detection candidates
//
// - Implements a lazy evaluation approach (i.e. the previous candidate is
// emitted once the next candidate arrives).
// - Implements trigger facilities
class DetectionCandidateProcessor {
 public:
  explicit DetectionCandidateProcessor(DetectionProcessor&& detectionProcessor,
                                       const Detector* detector);

  // Enables trigger facilities
  void enableTrigger(const boost::optional<Core::TimeSpan>& duration);
  // Sets the trigger thresholds
  void setTriggerThresholds(boost::optional<double> triggerOn,
                            boost::optional<double> triggerOff = boost::none);

  // Returns whether the processor is currently triggered (`true`) or whether
  // not `false`
  bool triggered() const;

  // Feeds a `candidate` to the processor. Blocking implementation.
  void feed(DetectionCandidate&& candidate);
  // Flushes the processor such that the pending detection candidate is
  // processed
  void flush();
  // Resets the processor
  void reset();

  // Allows access to the underlying `DetectionProcessor`
  const DetectionProcessor& detectionProcessor();

  using OnTriggeredCallback =
      std::function<void(const DetectionCandidateProcessor*,
                         const DetectionCandidate& triggerCandidate)>;
  void setOnTriggeredCallback(OnTriggeredCallback callback);

  using EmitDetectionCallback =
      std::function<void(std::unique_ptr<Detection> detection)>;
  void setEmitDetectionCallback(EmitDetectionCallback callback);

 private:
  // Processes the `candidate`
  void processCandidate(DetectionCandidate&& candidate);

  // Resets the trigger
  void resetTrigger();

  // Emits the detection
  void emitDetection(std::unique_ptr<Detection> detection);

  DetectionProcessor _detectionProcessor;

  OnTriggeredCallback _onTriggeredCallback;
  EmitDetectionCallback _emitDetectionCallback;

  // The current detection candidate
  boost::optional<DetectionCandidate> _currentDetectionCandidate;

  boost::optional<Core::TimeSpan> _triggerDuration;
  boost::optional<Core::Time> _triggerEnd;
  // The underlying processor used for triggering (i.e. the current reference
  // processor)
  boost::optional<std::string> _triggerProcId;
  boost::optional<double> _triggerOnThreshold;
  boost::optional<double> _triggerOffThreshold;

  const Detector* _detector{nullptr};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETECTIONCANDIDATEPROCESSOR_H_
