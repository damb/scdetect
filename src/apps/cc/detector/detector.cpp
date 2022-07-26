#include "detector.h"

#include <seiscomp/client/inventory.h>

#include <algorithm>
#include <boost/variant2/variant.hpp>

#include "../eventstore.h"
#include "../log.h"
#include "../processing/exception.h"
#include "../processing/util.h"
#include "../settings.h"
#include "../util/memory.h"
#include "../util/waveform_stream_id.h"
#include "event/link.h"
#include "event/record.h"
#include "inventory.h"
#include "linker/association.h"
#include "seiscomp/core/datetime.h"
#include "seiscomp/core/exceptions.h"
#include "template_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

Detector::Builder::Builder(const std::string &originId)
    : _origin{
          EventStore::Instance().getWithChildren<DataModel::Origin>(originId)} {
  if (!_origin) {
    throw builder::BaseException{"error while assigning origin: origin (" +
                                 originId + ") not found"};
  }
  // XXX(damb): Using `new` to access a non-public ctor; see also
  // https://abseil.io/tips/134
  setProduct(std::unique_ptr<Detector>{new Detector{}});
  setTemplateOrigin();
}

Detector::Builder &Detector::Builder::setId(const std::string &id) {
  product()->setId(id);
  return *this;
}

Detector::Builder &Detector::Builder::setConfig(
    const config::PublishConfig &publishConfig,
    const config::DetectorConfig &detectorConfig, bool playback) {
  product()->_config = detectorConfig;

  product()->enable(detectorConfig.enabled);

  _detectionProcessor.publishConfig = publishConfig;
  _detectionProcessor.timeCorrection = detectorConfig.timeCorrection;

  /* product()->setGapThreshold(Core::TimeSpan{detectorConfig.gapThreshold}); */
  /* product()->setGapTolerance(Core::TimeSpan{detectorConfig.gapTolerance}); */
  /* product()->setGapInterpolation(detectorConfig.gapInterpolation); */

  setMergingStrategy(detectorConfig.mergingStrategy);

  // configure playback related facilities
  if (playback) {
    product()->setMaxLatency(boost::none);
  } else {
    if (detectorConfig.maximumLatency > 0) {
      product()->setMaxLatency(Core::TimeSpan{detectorConfig.maximumLatency});
    }
  }

  return *this;
}

Detector::Builder &Detector::Builder::setStream(
    const std::string &waveformStreamId,
    const config::StreamConfig &streamConfig,
    WaveformHandlerIface *waveformHandler) {
  const auto &templateStreamId{streamConfig.templateConfig.wfStreamId};
  util::WaveformStreamID templateWfStreamId{templateStreamId};

  logging::TaggedMessage msg{waveformStreamId + " (" + templateStreamId + ")"};
  // configure pick from arrival
  DataModel::PickPtr pick;
  DataModel::WaveformStreamID pickWaveformId;
  DataModel::ArrivalPtr arrival;
  for (size_t i = 0; i < _origin->arrivalCount(); ++i) {
    arrival = _origin->arrival(i);

    if (arrival->phase().code() != streamConfig.templateConfig.phase) {
      continue;
    }

    pick = EventStore::Instance().get<DataModel::Pick>(arrival->pickID());
    if (!pick) {
      SCDETECT_LOG_DEBUG("Failed to load pick with id: %s",
                         arrival->pickID().c_str());
      continue;
    }
    if (!isValidArrival(*arrival, *pick)) {
      continue;
    }

    // compare sensor locations
    try {
      pick->time().value();
    } catch (...) {
      continue;
    }
    auto templateWfSensorLocation{
        Client::Inventory::Instance()->getSensorLocation(
            templateWfStreamId.netCode(), templateWfStreamId.staCode(),
            templateWfStreamId.locCode(), pick->time().value())};
    if (!templateWfSensorLocation) {
      msg.setText("sensor location not found in inventory for time: " +
                  pick->time().value().iso());
      throw builder::NoSensorLocation{logging::to_string(msg)};
    }
    pickWaveformId = pick->waveformID();
    auto pickWfSensorLocation{Client::Inventory::Instance()->getSensorLocation(
        pickWaveformId.networkCode(), pickWaveformId.stationCode(),
        pickWaveformId.locationCode(), pick->time().value())};
    if (!pickWfSensorLocation ||
        *templateWfSensorLocation != *pickWfSensorLocation) {
      continue;
    }

    break;
  }

  if (!pick) {
    arrival.reset();
    msg.setText("failed to load pick: origin=" + _origin->publicID() +
                ", phase=" + streamConfig.templateConfig.phase);
    throw builder::NoPick{logging::to_string(msg)};
  }

  msg.setText("using arrival pick: origin=" + _origin->publicID() +
              ", time=" + pick->time().value().iso() +
              ", phase=" + streamConfig.templateConfig.phase + ", stream=" +
              util::to_string(util::WaveformStreamID{pickWaveformId}));
  SCDETECT_LOG_DEBUG("%s", logging::to_string(msg).c_str());

  auto templateWaveformStartTime{
      pick->time().value() +
      Core::TimeSpan{streamConfig.templateConfig.wfStart}};
  auto templateWaveformEndTime{
      pick->time().value() + Core::TimeSpan{streamConfig.templateConfig.wfEnd}};

  // load stream metadata from inventory
  util::WaveformStreamID wfStreamId{waveformStreamId};
  auto *stream{Client::Inventory::Instance()->getStream(
      wfStreamId.netCode(), wfStreamId.staCode(), wfStreamId.locCode(),
      wfStreamId.chaCode(), templateWaveformStartTime)};

  if (!stream) {
    msg.setText("failed to load stream from inventory: start=" +
                templateWaveformStartTime.iso() +
                ", end=" + templateWaveformEndTime.iso());
    throw builder::NoStream{logging::to_string(msg)};
  }

  msg.setText("loaded stream from inventory for epoch: start=" +
              templateWaveformStartTime.iso() +
              ", end=" + templateWaveformEndTime.iso());
  SCDETECT_LOG_DEBUG("%s", logging::to_string(msg).c_str());

  const auto templateProcessorId{
      product()->id().empty() ? streamConfig.templateId
                              : product()->id() + settings::kProcessorIdSep +
                                    streamConfig.templateId};
  msg.setText("creating template waveform processor with id: " +
              templateProcessorId);
  SCDETECT_LOG_DEBUG("%s", logging::to_string(msg).c_str());

  // template related filter configuration (used for template waveform
  // processing)
  TemplateWaveform::ProcessingConfig processingConfig;
  processingConfig.templateStartTime = templateWaveformStartTime;
  processingConfig.templateEndTime = templateWaveformEndTime;
  processingConfig.safetyMargin = settings::kTemplateWaveformResampleMargin;
  processingConfig.detrend = false;
  processingConfig.demean = true;

  auto pickFilterId{pick->filterID()};
  auto templateWfFilterId{
      streamConfig.templateConfig.filter.value_or(pickFilterId)};
  if (!templateWfFilterId.empty()) {
    util::replaceEscapedXMLFilterIdChars(templateWfFilterId);
    processingConfig.filter = templateWfFilterId;
    processingConfig.initTime = Core::TimeSpan{streamConfig.initTime};
  }

  TemplateWaveform templateWaveform;
  try {
    templateWaveform = TemplateWaveform::load(
        waveformHandler, templateWfStreamId.netCode(),
        templateWfStreamId.staCode(), templateWfStreamId.locCode(),
        templateWfStreamId.chaCode(), processingConfig);

    templateWaveform.setReferenceTime(pick->time().value());

  } catch (WaveformHandler::NoData &e) {
    msg.setText("failed to load template waveform: " + std::string{e.what()});
    throw builder::NoWaveformData{logging::to_string(msg)};
  }

  TemplateProcessor templateProcessor{std::move(templateWaveform), product()};
  templateProcessor.setId(templateProcessorId);

  std::string rtFilterId{streamConfig.filter.value_or(pickFilterId)};
  // configure template related filter (used during real-time stream
  // processing)
  if (!rtFilterId.empty()) {
    util::replaceEscapedXMLFilterIdChars(rtFilterId);
    try {
      templateProcessor.setFilter(processing::createFilter<double>(rtFilterId),
                                  Core::TimeSpan{streamConfig.initTime});
    } catch (processing::BaseException &e) {
      msg.setText(e.what());
      throw builder::BaseException{logging::to_string(msg)};
    }
  }

  if (streamConfig.targetSamplingFrequency) {
    // TODO(damb): implement resampling facilities
    /* templateProcessor->setTargetSamplingFrequency( */
    /*     *streamConfig.targetSamplingFrequency); */
  }

  std::string text{"filters configured: filter=\"" + rtFilterId + "\""};
  if (rtFilterId != templateWfFilterId) {
    text += " (template_filter=\"" + templateWfFilterId + "\")";
  }
  msg.setText(text);
  SCDETECT_LOG_DEBUG_TAGGED(templateProcessor.id(), "%s",
                            logging::to_string(msg).c_str());

  TemplateProcessorConfig c{std::move(templateProcessor),
                            streamConfig.mergingThreshold,
                            {stream->sensorLocation(), pick, arrival}};

  _processorConfigs.emplace(waveformStreamId, std::move(c));

  return *this;
}

void Detector::Builder::finalize() {
  auto hasNoChildren{_processorConfigs.empty()};
  if (hasNoChildren) {
    product()->enable(false);
  }

  std::unordered_set<std::string> usedPicks;
  for (auto &procConfigPair : _processorConfigs) {
    const auto &waveformStreamId{procConfigPair.first};
    auto &procConfig{procConfigPair.second};

    // configure gap interpolation
    procConfig.processor.setGapThreshold(
        Core::TimeSpan{product()->_config.gapThreshold});
    procConfig.processor.setGapTolerance(
        Core::TimeSpan{product()->_config.gapTolerance});
    procConfig.processor.setGapInterpolation(
        product()->_config.gapInterpolation);

    const auto &meta{procConfig.metadata};
    boost::optional<std::string> phase_hint;
    try {
      phase_hint = meta.pick->phaseHint();
    } catch (Core::ValueException &e) {
    }

    detector::Arrival templateArrival{
        {meta.pick->time().value(), meta.pick->waveformID(), phase_hint,
         meta.pick->time().value() - _origin->time().value()},
        meta.arrival->phase(),
        meta.arrival->weight(),
    };
    detector::SensorLocation templateSensorLocation{
        detector::Station{meta.sensorLocation->station()->publicID()},
        meta.sensorLocation->publicID(),
        meta.sensorLocation->latitude(),
        meta.sensorLocation->longitude(),
    };

    product()->registerTemplateProcessor(
        std::move(procConfig.processor), waveformStreamId, templateArrival,
        templateSensorLocation, procConfig.mergingThreshold);

    const auto &templateProcessorId{procConfig.processor.id()};
    _detectionProcessor.templateProcessorInfos.emplace(
        templateProcessorId,
        DetectionProcessor::TemplateProcessorInfo{templateSensorLocation,
                                                  templateArrival.pick.time});
    usedPicks.emplace(meta.pick->publicID());
  }

  auto &publishConfig{_detectionProcessor.publishConfig};
  // attach reference theoretical template arrivals to the product
  if (publishConfig.createTemplateArrivals) {
    for (size_t i = 0; i < _origin->arrivalCount(); ++i) {
      const auto &arrival{_origin->arrival(i)};
      const auto &pick{
          EventStore::Instance().get<DataModel::Pick>(arrival->pickID())};

      bool isDetectorArrival{usedPicks.find(arrival->pickID()) !=
                             usedPicks.end()};
      if (isDetectorArrival || arrival->phase().code().empty()) {
        continue;
      }

      if (!pick) {
        SCDETECT_LOG_DEBUG("Failed to load pick with id: %s",
                           arrival->pickID().c_str());
        continue;
      }

      if (!isValidArrival(*arrival, *pick)) {
        continue;
      }

      boost::optional<std::string> phaseHint;
      try {
        phaseHint = pick->phaseHint();
      } catch (Core::ValueException &e) {
      }
      boost::optional<double> lowerUncertainty;
      try {
        lowerUncertainty = pick->time().lowerUncertainty();
      } catch (Core::ValueException &e) {
      }
      boost::optional<double> upperUncertainty;
      try {
        upperUncertainty = pick->time().upperUncertainty();
      } catch (Core::ValueException &e) {
      }

      publishConfig.theoreticalTemplateArrivals.push_back(
          {{pick->time().value(),
            util::to_string(util::WaveformStreamID{pick->waveformID()}),
            phaseHint, pick->time().value() - _origin->time().value(),
            lowerUncertainty, upperUncertainty},
           arrival->phase(),
           arrival->weight()});
    }
  }

  auto &detectorConfig{product()->detectorConfig()};
  if (detectorConfig.triggerDuration >= 0) {
    product()->_detectionCandidateProcessor.enableTrigger(
        Core::TimeSpan{detectorConfig.triggerDuration});
  }

  product()->_detectionCandidateProcessor.setTriggerThresholds(
      detectorConfig.triggerOn, detectorConfig.triggerOff);
  product()->_linker.setAssociationThreshold(detectorConfig.triggerOn);

  if (detectorConfig.arrivalOffsetThreshold < 0) {
    product()->_linker.setArrivalOffsetThreshold(boost::none);
  } else {
    product()->_linker.setArrivalOffsetThreshold(
        Core::TimeSpan{detectorConfig.arrivalOffsetThreshold});
  }

  if (detectorConfig.minArrivals < 0) {
    product()->_linker.setMinArrivals(boost::none);
  } else {
    product()->_linker.setMinArrivals(detectorConfig.minArrivals);
  }

  product()->_detectionCandidateProcessor.setProcessDetectionCallback(
      _detectionProcessor);
}

bool Detector::Builder::isValidArrival(const DataModel::Arrival &arrival,
                                       const DataModel::Pick &pick) {
  // check if both pick and arrival are properly configured
  try {
    if (pick.evaluationMode() != DataModel::MANUAL &&
        (arrival.weight() == 0 || !arrival.timeUsed())) {
      return false;
    }
  } catch (Core::ValueException &e) {
    return false;
  }
  return true;
}

void Detector::Builder::setTemplateOrigin() {
  auto &templateOrigin{_detectionProcessor.templateOrigin};
  templateOrigin.id = _origin->publicID();
  templateOrigin.latitude = _origin->latitude().value();
  templateOrigin.longitude = _origin->longitude().value();

  try {
    templateOrigin.depth = _origin->depth().value();
  } catch (Seiscomp::Core::ValueException &) {
  }
}

void Detector::Builder::setMergingStrategy(const std::string &strategyId) {
  if ("all" == strategyId) {
    product()->_linker.setMergingStrategy(
        [](const linker::Association::TemplateResult &result,
           double associationThreshold,
           double mergingThreshold) { return true; });
  } else if ("greaterEqualTriggerOnThreshold" == strategyId) {
    product()->_linker.setMergingStrategy(
        [](const linker::Association::TemplateResult &result,
           double associationThreshold, double mergingThreshold) {
          return result.resultIt->coefficient >= associationThreshold;
        });

  } else if ("greaterEqualMergingThreshold" == strategyId) {
    product()->_linker.setMergingStrategy(
        [](const linker::Association::TemplateResult &result,
           double associationThreshold, double mergingThreshold) {
          return result.resultIt->coefficient >= mergingThreshold;
        });
  } else {
    throw builder::BaseException{"invalid merging strategy: " + strategyId};
  }
}

/* ------------------------------------------------------------------------- */
Detector::Detector() : _detectionCandidateProcessor{this} {
  _linker.setResultCallback([this](linker::Association &&association) {
    onLinkerResultCallback(std::move(association));
  });
}

Detector::Builder Detector::Create(const std::string &originId) {
  return Builder(originId);
}

Detector::const_iterator Detector::begin() const noexcept {
  return std::begin(_templateProcessors);
}
Detector::const_iterator Detector::end() const noexcept {
  return std::end(_templateProcessors);
}
Detector::const_iterator Detector::cbegin() const noexcept {
  return _templateProcessors.cbegin();
}
Detector::const_iterator Detector::cend() const noexcept {
  return _templateProcessors.cend();
}

std::size_t Detector::size() const noexcept {
  return _templateProcessors.size();
}

bool Detector::empty() const noexcept { return !size(); }

void Detector::enable(bool enable) { _enabled = enable; }

bool Detector::enabled() const { return _enabled; }

Detector::Status Detector::status() const { return _status; }

void Detector::close() {
  for (auto &templateProcessor : _templateProcessors) {
    templateProcessor.close();
    templateProcessor.flush();
  }

  _status = Status::kClosed;
}

bool Detector::finished() const {
  return (_status == Status::kClosed &&
          std::all_of(std::begin(_templateProcessors),
                      std::end(_templateProcessors),
                      [](const TemplateProcessors::value_type &p) {
                        return p.finished();
                      })) ||
         _status > Status::kClosed;
}

void Detector::terminate() {
  SCDETECT_LOG_DEBUG_PROCESSOR(this, "Terminating ...");
  flush();

  _status = Status::kTerminated;
}

void Detector::dispatch(Event &&ev) {
  boost::variant2::visit(EventHandler{this}, std::move(ev));
}

void Detector::postEvent(Event &&ev) const {
  assert(_emitEventCallback);
  _emitEventCallback(std::move(ev));
}

void Detector::reset() {
  for (auto &templateProcessor : _templateProcessors) {
    templateProcessor.reset();
  }
  resetTemplateProcessorLinkingInfo();

  _linker.reset();
  _detectionCandidateProcessor.reset();

  _status = Status::kWaitingForData;
}

void Detector::flush() {
  // XXX(damb): flush pending linker associations. This is possible due to
  // feeding linker associations straight away to the detection candidate
  // processor, i.e. candidates are processed synchronously.
  _linker.flush();
  _detectionCandidateProcessor.flush();
}

const TemplateProcessor *Detector::processor(
    const std::string &processorId) const {
  auto it{_templateProcessorIdIdx.find(processorId)};
  if (it == _templateProcessorIdIdx.end()) {
    return nullptr;
  }

  return &_templateProcessors[it->second];
}

const config::DetectorConfig &Detector::detectorConfig() const {
  return _config;
}

void Detector::setEmitDetectionCallback(EmitDetectionCallback callback) {
  _detectionCandidateProcessor.setEmitDetectionCallback(
      [callback, this](std::unique_ptr<Detection> detection) {
        // re-enable all template processors
        resetTemplateProcessorLinkingInfo();

        // emit detection
        callback(this, std::move(detection));
      });
}

void Detector::setEmitEventCallback(EmitEventCallback callback) {
  _emitEventCallback = std::move(callback);
}

std::set<std::string> Detector::associatedWaveformStreamIds() const {
  const auto &waveformStreamIds{
      util::map_keys(_templateProcessorWaveformStreamIdIdx)};
  return std::set<std::string>{std::begin(waveformStreamIds),
                               std::end(waveformStreamIds)};
}

const Linker &Detector::linker() const { return _linker; }

void Detector::setMaxLatency(boost::optional<Core::TimeSpan> latency) {
  _maxLatency = latency;
}

const boost::optional<Core::TimeSpan> &Detector::maxLatency() const {
  return _maxLatency;
}

Detector::EventHandler::EventHandler(Detector *detector) : detector{detector} {}

void Detector::EventHandler::operator()(event::Link &&ev) {
  detector->link(ev.templateProcessorId, std::move(ev.matchResult));
}

void Detector::EventHandler::operator()(InternalEvent &&ev) {
  boost::variant2::visit(InternalEventHandler{detector}, std::move(ev));
}

Detector::InternalEventHandler::InternalEventHandler(Detector *detector)
    : detector{detector} {}

void Detector::registerTemplateProcessor(
    TemplateProcessor &&templateProcessor,
    /* const boost::optional<Core::TimeSpan> &templateProcessorBufferSize, */
    const std::string &waveformStreamId, const Arrival &arrival,
    const SensorLocation &sensorLocation,
    const boost::optional<double> &mergingThreshold) {
  // XXX(damb): Replace the arrival with a *pseudo arrival* i.e. an arrival
  // which is associated with the stream to be processed
  Arrival pseudoArrival{arrival};
  pseudoArrival.pick.waveformStreamId = waveformStreamId;

  const auto templateProcessorId{templateProcessor.id()};

  _templateProcessors.emplace_back(std::move(templateProcessor));
  _templateProcessorLinkingInfo.emplace_back(true);
  _templateProcessorIdIdx.emplace(templateProcessorId,
                                  _templateProcessors.size() - 1);
  _templateProcessorWaveformStreamIdIdx.emplace(waveformStreamId,
                                                _templateProcessors.size() - 1);

  _linker.registerTemplateProcessor(templateProcessorId, pseudoArrival,
                                    mergingThreshold);
  const auto onHoldDuration{_maxLatency.value_or(0.0) +
                            templateProcessor.initTime().value_or(0.0) +
                            /*templateProcessorBufferSize.value_or(0.0) + */
                            Linker::DefaultSafetyMargin};

  if (_linker.onHold() < onHoldDuration) {
    _linker.setOnHold(onHoldDuration);
  }
}

bool Detector::hasAcceptableLatency(const Record *record) const {
  if (_maxLatency) {
    return record->endTime() > Core::Time::GMT() - *_maxLatency;
  }

  return true;
}

void Detector::onTriggeredCallback(
    const DetectionCandidateProcessor *detectionCandidateProcessor,
    const DetectionCandidate &candidate) {
  // enable all
  resetTemplateProcessorLinkingInfo();

  const auto contributing{util::map_keys(candidate.results)};
  const auto all{util::map_keys(_templateProcessorIdIdx)};

  std::vector<detail::ProcessorIdType> notContributing;
  std::set_difference(
      std::begin(all), std::end(all), std::begin(contributing),
      std::end(contributing),
      std::inserter(notContributing, std::begin(notContributing)));

  for (const auto &templateProcessorId : notContributing) {
    _templateProcessorLinkingInfo
        [_templateProcessorIdIdx[templateProcessorId]] = false;
  }
}

void Detector::onLinkerResultCallback(linker::Association &&association) {
  _detectionCandidateProcessor.feed(std::move(association));
}

void Detector::link(const detail::ProcessorIdType &templateProcessorId,
                    MatchResult &&matchResult) {
  auto idx{_templateProcessorIdIdx[templateProcessorId]};
  auto &templateProcessor{_templateProcessors[idx]};

  bool linkingEnabled{_templateProcessorLinkingInfo[idx]};
  if (linkingEnabled) {
    _linker.feed(templateProcessor, std::move(matchResult));
  }

  event::Finished finished;
  finished.initialized = true;
  TemplateProcessor::Event nextEvent{finished};
  templateProcessor.dispatch(std::move(nextEvent));
}

void Detector::dispatch(event::Record &&ev) {
  assert(ev.record);

  auto *record{ev.record.get()};
  if (!hasAcceptableLatency(record)) {
    logging::TaggedMessage msg{
        ev.record->streamID(),
        "record exceeds acceptable latency. Dropping record (start=" +
            record->startTime().iso() + ", end=" + record->endTime().iso() +
            ")"};
    SCDETECT_LOG_WARNING_PROCESSOR(this, "%s", logging::to_string(msg).c_str());
    // nothing to do
    return;
  }

  const auto templateProcessorIdxs{
      _templateProcessorWaveformStreamIdIdx.equal_range(record->streamID())};
  for (auto i{templateProcessorIdxs.first}; i != templateProcessorIdxs.second;
       ++i) {
    auto &templateProcessor{_templateProcessors[i->second]};

    // XXX(damb): clone. The record might be used by multiple template
    // processors.
    templateProcessor.dispatch(event::Record{ev});
    if (templateProcessor.finished()) {
      templateProcessor.reset();
    }
  }
}

void Detector::resetTemplateProcessorLinkingInfo() {
  std::fill(std::begin(_templateProcessorLinkingInfo),
            std::end(_templateProcessorLinkingInfo), true);
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
