#include "linker.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <unordered_set>

#include "../util/math.h"
#include "../util/util.h"
#include "detail.h"
#include "match_result.h"

namespace Seiscomp {
namespace detect {
namespace detector {

Linker::Linker(const Core::TimeSpan &onHold,
               const Core::TimeSpan &arrivalOffsetThres)
    : _thresArrivalOffset{arrivalOffsetThres}, _onHold{onHold} {}

void Linker::setArrivalOffsetThreshold(
    const boost::optional<Core::TimeSpan> &thres) {
  _thresArrivalOffset = thres;
}

boost::optional<Core::TimeSpan> Linker::arrivalOffsetThreshold() const {
  return _thresArrivalOffset;
}

void Linker::setAssociationThreshold(const boost::optional<double> &thres) {
  _thresAssociation = thres;
}

boost::optional<double> Linker::associationThreshold() const {
  return _thresAssociation;
}

void Linker::setMinArrivals(const boost::optional<size_t> &n) {
  auto v{n};
  if (v && 1 > *v) {
    v = boost::none;
  }

  _minArrivals = v;
}

boost::optional<size_t> Linker::minArrivals() const { return _minArrivals; }

void Linker::setOnHold(const Core::TimeSpan &duration) { _onHold = duration; }

Core::TimeSpan Linker::onHold() const { return _onHold; }

void Linker::setMergingStrategy(MergingStrategy mergingStrategy) {
  _mergingStrategy = std::move(mergingStrategy);
}

size_t Linker::channelCount() const {
  std::unordered_set<std::string> wfIds;
  for (const auto &procPair : _templateProcessorInfos) {
    wfIds.emplace(procPair.second.arrival.pick.waveformStreamId);
  }

  return wfIds.size();
}

std::size_t Linker::size() const noexcept {
  return _templateProcessorInfos.size();
}

void Linker::registerTemplateProcessor(
    const detail::ProcessorIdType &templateProcessorId, const Arrival &arrival,
    const boost::optional<double> &mergingThreshold) {
  _templateProcessorInfos.emplace(
      templateProcessorId, TemplateProcessorInfo{arrival, mergingThreshold});
  _potValid = false;
}

void Linker::unregisterTemplateProcessor(
    const std::string &templateProcessorId) {
  _templateProcessorInfos.erase(templateProcessorId);
  _potValid = false;
}

void Linker::reset() {
  _queue.clear();
  _potValid = false;
}

void Linker::flush() {
  // flush pending events
  while (!_queue.empty()) {
    auto candidate{_queue.front()};
    if (candidate.associatedProcessorCount() >= _minArrivals.value_or(size()) &&
        (!_thresAssociation ||
         candidate.association.score >= *_thresAssociation)) {
      emitResult(std::move(candidate.association));
    }

    _queue.pop_front();
  }
}

void Linker::feed(const TemplateProcessor &templateProcessor,
                  MatchResult &&matchResult) {
  auto it{_templateProcessorInfos.find(templateProcessor.id())};
  if (it == _templateProcessorInfos.end()) {
    return;
  }

  auto &templateProcessorInfo{it->second};
  // create a new arrival from a *template arrival*
  auto newArrival{templateProcessorInfo.arrival};

  auto result{std::make_shared<const MatchResult>(std::move(matchResult))};
  // XXX(damb): recompute the pickOffset; the template proc might have
  // changed the underlying template waveform (due to resampling)
  const auto currentPickOffset{
      templateProcessorInfo.arrival.pick.time -
      templateProcessor.templateWaveform().startTime()};
  for (auto valueIt{result->localMaxima.begin()};
       valueIt != result->localMaxima.end(); ++valueIt) {
    const auto time{result->timeWindow.startTime() + valueIt->lag +
                    currentPickOffset};
    newArrival.pick.time = time;

    linker::Association::TemplateResult templateResult{newArrival, valueIt,
                                                       result};
    // filter/drop based on merging strategy
    if (_thresAssociation &&
        !_mergingStrategy(templateResult, *_thresAssociation,
                          templateProcessorInfo.mergingThreshold.value_or(
                              *_thresAssociation))) {
#ifdef SCDETECT_DEBUG
      SCDETECT_LOG_DEBUG_PROCESSOR(
          proc,
          "[%s] [%s - %s] Dropping result due to merging "
          "strategy applied: time=%s, score=%9f, lag=%10f",
          newArrival.pick.waveformStreamId.c_str(),
          result->timeWindow.startTime().iso().c_str(),
          result->timeWindow.endTime().iso().c_str(), time.iso().c_str(),
          valueIt->coefficient, static_cast<double>(valueIt->lag));
#endif
      continue;
    }

#ifdef SCDETECT_DEBUG
    SCDETECT_LOG_DEBUG_PROCESSOR(
        proc,
        "[%s] [%s - %s] Trying to merge result: time=%s, score=%9f, lag=%10f",
        newArrival.pick.waveformStreamId.c_str(),
        result->timeWindow.startTime().iso().c_str(),
        result->timeWindow.endTime().iso().c_str(), time.iso().c_str(),
        valueIt->coefficient, static_cast<double>(valueIt->lag));
#endif
    process(templateProcessor, templateResult);
  }
}

void Linker::setResultCallback(PublishResultCallback callback) {
  _resultCallback = std::move(callback);
}

void Linker::process(const TemplateProcessor &templateProcessor,
                     const linker::Association::TemplateResult &result) {
  // update POT
  if (!_potValid) {
    createPot();
  }

  const auto &templateProcessorId{templateProcessor.id()};
  auto resultIt{result.resultIt};
  // merge result into existing candidates
  for (auto candidateIt = std::begin(_queue); candidateIt != std::end(_queue);
       ++candidateIt) {
    if (candidateIt->associatedProcessorCount() < size()) {
      auto &candidateTemplateResults{candidateIt->association.results};
      auto it{candidateTemplateResults.find(templateProcessorId)};

      bool newPick{it == candidateTemplateResults.end()};
      if (newPick || resultIt->coefficient > it->second.resultIt->coefficient) {
        if (_thresArrivalOffset) {
          auto candidatePOTData{createCandidatePOTData(
              *candidateIt, templateProcessorId, result)};
          if (!_pot.validateEnabledOffsets(
                  templateProcessorId, candidatePOTData.offsets,
                  candidatePOTData.mask, *_thresArrivalOffset)) {
            continue;
          }
        }
        candidateIt->feed(templateProcessorId, result);
      }
    }
  }

  const auto now{Core::Time::GMT()};
  // create new candidate association
  Candidate newCandidate{now + _onHold};
  newCandidate.feed(templateProcessorId, result);
  _queue.emplace_back(newCandidate);

  std::vector<CandidateQueue::iterator> ready;
  for (auto it = std::begin(_queue); it != std::end(_queue); ++it) {
    const auto arrivalCount{it->associatedProcessorCount()};
    // emit results which are ready and surpass threshold
    if (arrivalCount == size() ||
        (now >= it->expired && arrivalCount >= _minArrivals.value_or(size()))) {
      if (!_thresAssociation || it->association.score >= *_thresAssociation) {
        emitResult(std::move(it->association));
      }
      ready.push_back(it);
    }
    // drop expired result
    else if (now >= it->expired) {
      ready.push_back(it);
    }
  }

  // clean up result queue
  for (auto &it : ready) {
    _queue.erase(it);
  }
}

void Linker::emitResult(linker::Association &&result) {
  if (_resultCallback) {
    _resultCallback(std::move(result));
  }
}

void Linker::createPot() {
  std::vector<linker::POT::Entry> entries;
  using pair_type = TemplateProcessorInfos::value_type;
  std::transform(
      _templateProcessorInfos.cbegin(), _templateProcessorInfos.cend(),
      back_inserter(entries), [](const pair_type &p) {
        return linker::POT::Entry{p.second.arrival.pick.time, p.first, true};
      });

  // XXX(damb): the current implementation simply recreates the POT
  _pot = linker::POT(entries);
  _potValid = true;
}

Linker::CandidatePOTData Linker::createCandidatePOTData(
    const Candidate &candidate, const detail::ProcessorIdType &processorId,
    const linker::Association::TemplateResult &newResult) {
  auto allProcessorIds{_pot.processorIds()};
  const auto &associatedCandidateTemplateResults{candidate.association.results};

  auto associatedProcessorIds{
      util::map_keys(associatedCandidateTemplateResults)};
  std::set<detail::ProcessorIdType> enabledProcessorIds{
      associatedProcessorIds.begin(), associatedProcessorIds.end()};
  enabledProcessorIds.emplace(processorId);

  CandidatePOTData ret(allProcessorIds.size());
  for (std::size_t i{0}; i < allProcessorIds.size(); ++i) {
    const auto &curProcessorId{allProcessorIds[i]};
    bool disabled{enabledProcessorIds.find(curProcessorId) ==
                  enabledProcessorIds.end()};
    if (disabled) {
      continue;
    }

    if (curProcessorId != processorId) {
      ret.offsets[i] = std::abs(static_cast<double>(
          associatedCandidateTemplateResults.at(curProcessorId)
              .arrival.pick.time -
          newResult.arrival.pick.time));
    } else {
      ret.offsets[i] = 0;
    }
    ret.mask[i] = true;
  }

  return ret;
}

/* ------------------------------------------------------------------------- */
Linker::Candidate::Candidate(const Core::Time &expired) : expired{expired} {}

void Linker::Candidate::feed(const detail::ProcessorIdType &templateProcessorId,
                             const linker::Association::TemplateResult &res) {
  auto &templateResults{association.results};
  templateResults.emplace(templateProcessorId, res);

  std::vector<double> scores;
  std::transform(std::begin(templateResults), std::end(templateResults),
                 std::back_inserter(scores),
                 [](const linker::Association::TemplateResults::value_type &p) {
                   return p.second.resultIt->coefficient;
                 });

  // compute the overall event's score
  association.score = util::cma(scores.data(), scores.size());
}

std::size_t Linker::Candidate::associatedProcessorCount() const noexcept {
  return association.processorCount();
}

bool Linker::Candidate::isExpired(const Core::Time &now) const {
  return now >= expired;
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
