#include "detection_candidate_processor.h"

#include <cassert>
#include <unordered_set>
#include <vector>

#include "../log.h"
#include "detail.h"
#include "detection_candidate.h"
#include "detector.h"

namespace Seiscomp {
namespace detect {
namespace detector {

DetectionCandidateProcessor::DetectionCandidateProcessor(
    const Detector *detector)
    : _detector{detector} {}

void DetectionCandidateProcessor::enableTrigger(
    const boost::optional<Core::TimeSpan> &duration) {
  _triggerDuration = duration;
}

void DetectionCandidateProcessor::setTriggerThresholds(
    boost::optional<double> triggerOn, boost::optional<double> triggerOff) {
  _triggerOnThreshold = triggerOn;

  if (_triggerOnThreshold) {
    _triggerOffThreshold = triggerOff;
  }
}

bool DetectionCandidateProcessor::triggered() const {
  return static_cast<bool>(_triggerEnd);
}

void DetectionCandidateProcessor::feed(DetectionCandidate &&candidate) {
  processCandidate(std::move(candidate));
}

void DetectionCandidateProcessor::flush() {
  assert(_processDetectionCallback);

  if (_currentDetectionCandidate) {
    emitDetection(
        _processDetectionCallback(_detector, *_currentDetectionCandidate));
  }
  reset();
}

void DetectionCandidateProcessor::reset() {
  _currentDetectionCandidate = boost::none;
  resetTrigger();
}

void DetectionCandidateProcessor::setOnTriggeredCallback(
    OnTriggeredCallback callback) {
  _onTriggeredCallback = std::move(callback);
}

void DetectionCandidateProcessor::setProcessDetectionCallback(
    ProcessDetectionCallback callback) {
  _processDetectionCallback = std::move(callback);
}

void DetectionCandidateProcessor::setEmitDetectionCallback(
    EmitDetectionCallback callback) {
  _emitDetectionCallback = std::move(callback);
}

void DetectionCandidateProcessor::processCandidate(
    DetectionCandidate &&candidate) {
  const auto triggerOnThreshold{_triggerOnThreshold.value_or(-1)};
  const auto triggerOffThreshold{_triggerOffThreshold.value_or(1)};

  const auto sorted{sortContributorsByArrivalTime(candidate)};
  const auto &earliestArrivalTemplateResult{sorted.at(0)};
  const auto &pickTime{
      earliestArrivalTemplateResult.templateResult.arrival.pick.time};

  bool newTrigger{false};
  bool updatedResult{false};
  if (candidate.score > triggerOnThreshold) {
    if (!_currentDetectionCandidate) {
      _currentDetectionCandidate = candidate;

      // enable trigger
      if (_triggerDuration && *_triggerDuration > Core::TimeSpan{0.0}) {
        SCDETECT_LOG_DEBUG_PROCESSOR(_detector,
                                     "Detector result (triggering) %s",
                                     candidate.debugString().c_str());

        if (_onTriggeredCallback) {
          _onTriggeredCallback(this, candidate);
        }

        _triggerProcId = earliestArrivalTemplateResult.templateProcessorId;
        _triggerEnd = pickTime + *_triggerDuration;

        newTrigger = true;
      } else {
        SCDETECT_LOG_DEBUG_PROCESSOR(_detector, "Detector result %s",
                                     candidate.debugString().c_str());
      }
    } else if (triggered() && (pickTime <= *_triggerEnd) &&
               candidate.score > _currentDetectionCandidate.value().score &&
               candidate.processorCount() >=
                   _currentDetectionCandidate.value().processorCount()) {
      SCDETECT_LOG_DEBUG_PROCESSOR(_detector,
                                   "Detector result (triggered, updating) %s",
                                   candidate.debugString().c_str());

      _currentDetectionCandidate = candidate;
      if (_onTriggeredCallback) {
        _onTriggeredCallback(this, candidate);
      }

      _triggerProcId = earliestArrivalTemplateResult.templateProcessorId;
      _triggerEnd = pickTime + *_triggerDuration;

      updatedResult = true;
    }
  }

  bool expired{false};
  if (triggered()) {
    expired = pickTime > *_triggerEnd;

    if (!expired && !newTrigger && !updatedResult &&
        candidate.score <= _currentDetectionCandidate.value().score &&
        candidate.score >= triggerOffThreshold) {
      SCDETECT_LOG_DEBUG_PROCESSOR(_detector,
                                   "Detector result (triggered, dropped) %s",
                                   candidate.debugString().c_str());
    }

    // disable trigger if required
    if (expired || candidate.score < triggerOffThreshold) {
      resetTrigger();
    }
  }

  // emit detection
  if (!triggered()) {
    assert(_processDetectionCallback);
    emitDetection(
        _processDetectionCallback(_detector, *_currentDetectionCandidate));
  }

  // re-trigger
  if (expired && candidate.score > triggerOnThreshold &&
      *_currentDetectionCandidate != candidate) {
    SCDETECT_LOG_DEBUG_PROCESSOR(_detector, "Detector result (triggering) %s",
                                 candidate.debugString().c_str());

    _currentDetectionCandidate = candidate;
    if (_onTriggeredCallback) {
      _onTriggeredCallback(this, candidate);
    }

    _triggerProcId = earliestArrivalTemplateResult.templateProcessorId;
    _triggerEnd = pickTime + *_triggerDuration;
  }

  if (!triggered()) {
    _currentDetectionCandidate = boost::none;
  }
}

void DetectionCandidateProcessor::resetTrigger() {
  _triggerProcId = boost::none;
  _triggerEnd = boost::none;
}

void DetectionCandidateProcessor::emitDetection(
    std::unique_ptr<Detection> detection) {
  if (_emitDetectionCallback) {
    _emitDetectionCallback(std::move(detection));
  }
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
