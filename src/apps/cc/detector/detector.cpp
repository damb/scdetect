#include "detector.h"

#include "../log.h"
#include "../util/memory.h"
#include "detectorbuilder.h"

namespace Seiscomp {
namespace detect {
namespace detector {

Detector::Detector(const DataModel::OriginCPtr &origin)
    : _detectorImpl{origin}, _origin{origin} {}

DetectorBuilder Detector::Create(const std::string &originId) {
  return DetectorBuilder(originId);
}

void Detector::setResultCallback(const PublishDetectionCallback &callback) {
  _detectionCallback = callback;
}

void Detector::reset() {
  SCDETECT_LOG_DEBUG_PROCESSOR(this, "Resetting detector ...");

  // reset template (child) related facilities
  for (auto &streamStatePair : _streamStates) {
    streamStatePair.second = WaveformProcessor::StreamState{};
  }

  _detectorImpl.reset();

  WaveformProcessor::reset();
}

void Detector::terminate() {
  SCDETECT_LOG_DEBUG_PROCESSOR(this, "Terminating ...");

  _detectorImpl.terminate();
  if (_detection) {
    auto detection{util::make_smart<Detection>()};
    prepareDetection(detection, *_detection);
    emitDetection(nullptr, detection);

    _detection = boost::none;
  }
  WaveformProcessor::terminate();
}

const config::PublishConfig &Detector::publishConfig() const {
  return _publishConfig;
}

const TemplateWaveformProcessor *Detector::processor(
    const std::string &processorId) const {
  return _detectorImpl.processor(processorId);
}

processing::WaveformProcessor::StreamState *Detector::streamState(
    const Record *record) {
  return &_streamStates.at(record->streamID());
}

void Detector::process(StreamState &streamState, const Record *record,
                       const DoubleArray &filteredData) {
  try {
    _detectorImpl.feed(record);
  } catch (detector::DetectorImpl::ProcessingError &e) {
    SCDETECT_LOG_WARNING_PROCESSOR(this, "%s: %s. Resetting.",
                                   record->streamID().c_str(), e.what());
    _detectorImpl.reset();
  } catch (std::exception &e) {
    SCDETECT_LOG_ERROR_PROCESSOR(this, "%s: unhandled exception: %s",
                                 record->streamID().c_str(), e.what());

    setStatus(WaveformProcessor::Status::kError, 0);
  } catch (...) {
    SCDETECT_LOG_ERROR_PROCESSOR(this, "%s: unknown exception",
                                 record->streamID().c_str());

    setStatus(WaveformProcessor::Status::kError, 0);
  }

  if (!finished()) {
    if (_detection) {
      auto detection{util::make_smart<Detection>()};
      prepareDetection(detection, *_detection);
      emitDetection(record, detection);

      _detection = boost::none;
    }
  }
}

void Detector::reset(StreamState &streamState) {
  // XXX(damb): drops all pending events
  _detectorImpl.reset();

  WaveformProcessor::reset(streamState);
}

bool Detector::fill(processing::StreamState &streamState, const Record *record,
                    DoubleArrayPtr &data) {
  // XXX(damb): `Detector` does neither implement filtering facilities nor does
  // it perform a saturation check
  auto &s = dynamic_cast<WaveformProcessor::StreamState &>(streamState);
  s.receivedSamples += data->size();

  return true;
}

void Detector::storeDetection(const detector::DetectorImpl::Result &res) {
  _detection = res;
}

void Detector::prepareDetection(DetectionPtr &d,
                                const detector::DetectorImpl::Result &res) {
  const Core::TimeSpan timeCorrection{_config.timeCorrection};

  d->fit = _detection.value().fit;
  d->time = res.originTime + timeCorrection;
  d->latitude = _origin->latitude().value();
  d->longitude = _origin->longitude().value();
  d->depth = _origin->depth().value();

  d->numChannelsAssociated = res.numChannelsAssociated;
  d->numChannelsUsed = res.numChannelsUsed;
  d->numStationsAssociated = res.numStationsAssociated;
  d->numStationsUsed = res.numStationsUsed;

  d->publishConfig.createArrivals = _publishConfig.createArrivals;
  d->publishConfig.createTemplateArrivals =
      _publishConfig.createTemplateArrivals;
  d->publishConfig.originMethodId = _publishConfig.originMethodId;
  d->publishConfig.createAmplitudes = _publishConfig.createAmplitudes;
  d->publishConfig.createMagnitudes = _publishConfig.createMagnitudes;

  if (_publishConfig.createTemplateArrivals) {
    for (const auto &arrival : _publishConfig.theoreticalTemplateArrivals) {
      auto theoreticalTemplateArrival{arrival};
      theoreticalTemplateArrival.pick.time =
          res.originTime + arrival.pick.offset + timeCorrection;
      d->publishConfig.theoreticalTemplateArrivals.push_back(
          theoreticalTemplateArrival);
    }
  }

  d->templateResults = res.templateResults;
  if (timeCorrection) {
    for (auto &templateResultPair : d->templateResults) {
      templateResultPair.second.arrival.pick.time += timeCorrection;
    }
  }
}
void Detector::emitDetection(const Record *record,
                             const DetectionCPtr &detection) {
  if (enabled() && _detectionCallback) {
    _detectionCallback(this, record, detection);
  }
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
