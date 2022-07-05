#include "detection_processor.h"

#include <unordered_set>

#include "../util/memory.h"
#include "detection_candidate.h"

namespace Seiscomp {
namespace detect {
namespace detector {

std::unique_ptr<Detection> DetectionProcessor::operator()(
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

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
