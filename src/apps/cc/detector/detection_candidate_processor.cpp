#include "detection_candidate_processor.h"

#include <unordered_set>
#include <vector>

#include "../log.h"
#include "../util/memory.h"
#include "detail.h"
#include "detector.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace {

struct TemplateResultInfo {
  DetectionCandidate::TemplateResult templateResult;
  detail::ProcessorIdType templateProcessorId;
};

std::vector<TemplateResultInfo> sortContributorsByArrivalTime(
    const DetectionCandidate &candidate) {
  std::vector<TemplateResultInfo> ret;
  for (const auto &resultPair : candidate.results) {
    ret.push_back(TemplateResultInfo{resultPair.second, resultPair.first});
  }
  std::sort(std::begin(ret), std::end(ret),
            [](const TemplateResultInfo &lhs, const TemplateResultInfo &rhs) {
              return lhs.templateResult.arrival.pick.time <
                     rhs.templateResult.arrival.pick.time;
            });
  return ret;
}

}  // namespace

std::unique_ptr<Detection>
DetectionCandidateProcessor::DetectionProcessor::operator()(
    const Detector *detector, const DetectionCandidate &candidate) const {
  assert(detector);
  auto sorted{sortContributorsByArrivalTime(candidate)};

  const auto &referenceResult{sorted.at(0)};
  const auto &referenceArrival{referenceResult.templateResult.arrival};
  const auto referenceMatchResult{referenceResult.templateResult.matchResult};
  const auto &referenceStartTime{referenceMatchResult->timeWindow.startTime()};
  const auto referenceLag{
      static_cast<double>(referenceArrival.pick.time - referenceStartTime)};

  std::unordered_set<std::string> usedChas;
  std::unordered_set<std::string> usedStas;
  std::vector<double> alignedArrivalOffsets;

  Detection::ContributionInfos contributionInfos;
  for (const auto &contribution : sorted) {
    const auto procId{contribution.templateProcessorId};
    const auto &templateResult{contribution.templateResult};

    if (templateResult.matchResult) {
      const auto matchResult{templateResult.matchResult};
      const auto &startTime{matchResult->timeWindow.startTime()};

      auto arrivalOffset{templateResult.arrival.pick.time -
                         referenceArrival.pick.time};
      // compute alignment correction using the POT offset (required, since
      // traces to be cross-correlated cannot be guaranteed to be aligned to
      // sub-sampling interval accuracy)
      const auto &alignmentCorrection{referenceStartTime + arrivalOffset -
                                      startTime};

      alignedArrivalOffsets.push_back(
          static_cast<double>(templateResult.arrival.pick.time - startTime) -
          alignmentCorrection - referenceLag);

      const auto *proc{detector->processor(procId)};
      const auto &templateProcessorInfo{templateProcessorInfos.at(procId)};

      auto arrival{templateResult.arrival};
      arrival.pick.time += timeCorrection;
      contributionInfos.emplace(
          procId, Detection::ContributionInfo{
                      arrival, templateProcessorInfo.sensorLocation,
                      proc->templateWaveform().startTime(),
                      proc->templateWaveform().endTime(),
                      templateProcessorInfo.templateWaveformReferenceTime});
      usedChas.emplace(templateResult.arrival.pick.waveformStreamId);
      usedStas.emplace(
          templateProcessorInfos.at(procId).sensorLocation.station.id);
    }
  }

  // compute origin time
  const auto &arrivalOffsetCorrection{
      util::cma(alignedArrivalOffsets.data(), alignedArrivalOffsets.size())};
  const auto &referenceOriginArrivalOffset{referenceArrival.pick.offset};

  auto ret{util::make_unique<Detection>(contributionInfos, publishConfig)};

  ret->origin.time = referenceStartTime + Core::TimeSpan{referenceLag} -
                     referenceOriginArrivalOffset +
                     Core::TimeSpan{arrivalOffsetCorrection} + timeCorrection;
  ret->origin.latitude = templateOrigin.latitude;
  ret->origin.longitude = templateOrigin.longitude;
  ret->origin.depth = templateOrigin.depth;

  ret->score = candidate.score;

  if (ret->publishConfig.createTemplateArrivals) {
    for (const auto &arrival : ret->publishConfig.theoreticalTemplateArrivals) {
      auto theoreticalTemplateArrival{arrival};
      theoreticalTemplateArrival.pick.time =
          ret->origin.time + arrival.pick.offset + timeCorrection;
      ret->publishConfig.theoreticalTemplateArrivals.push_back(
          theoreticalTemplateArrival);
    }
  }

  // number of channels used
  ret->numChannelsUsed = usedChas.size();
  // number of stations used
  ret->numStationsUsed = usedStas.size();
  // number of channels/stations associated
  std::unordered_set<std::string> associatedStations;
  for (const auto &procPair : templateProcessorInfos) {
    associatedStations.emplace(procPair.second.sensorLocation.station.id);
  }

  // TODO(damb): make linker accessible
  // ret->numChannelsAssociated = detector->linker().channelCount();
  ret->numStationsAssociated = associatedStations.size();

  return ret;
}

DetectionCandidateProcessor::DetectionCandidateProcessor(
    DetectionProcessor &&detectionProcessor, const Detector *detector)
    : _detectionProcessor{std::move(detectionProcessor)}, _detector{detector} {}

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
  if (_currentDetectionCandidate) {
    emitDetection(_detectionProcessor(_detector, *_currentDetectionCandidate));
  }
  reset();
}

void DetectionCandidateProcessor::reset() {
  _currentDetectionCandidate = boost::none;
  resetTrigger();
}

const DetectionCandidateProcessor::DetectionProcessor &
DetectionCandidateProcessor::detectionProcessor() {
  return _detectionProcessor;
}

void DetectionCandidateProcessor::setOnTriggeredCallback(
    OnTriggeredCallback callback) {
  _onTriggeredCallback = std::move(callback);
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
    emitDetection(_detectionProcessor(_detector, *_currentDetectionCandidate));
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

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
