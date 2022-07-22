#include "template_processor.h"

#include <boost/variant2/variant.hpp>
#include <memory>

#include "../def.h"
#include "detector.h"
#include "template_processor/state_machine.h"

namespace Seiscomp {
namespace detect {
namespace detector {

TemplateProcessor::TemplateProcessor(TemplateWaveform templateWaveform,
                                     Detector* parent)
    : _crossCorrelation{std::move(templateWaveform)}, _parent{parent} {}

bool TemplateProcessor::finished() const {
  return _status > Status::kWaitingForData;
}

void TemplateProcessor::reset() {
  reset(_streamState);
  _crossCorrelation.reset();

  _status = Status::kWaitingForData;
}

void TemplateProcessor::dispatch(Event&& ev) {
  boost::variant2::visit(EventHandler{this}, std::move(ev));
}

void TemplateProcessor::setFilter(
    std::unique_ptr<Filter> filter,
    const boost::optional<Core::TimeSpan>& initTime) {
  _streamState.filter = std::move(filter);
  _initTime = std::max(initTime.value_or(0.0),
                       templateWaveform().configuredEndTime() -
                           templateWaveform().configuredStartTime());
}

boost::optional<Core::TimeSpan> TemplateProcessor::initTime() const {
  return _initTime;
}

const Detector* TemplateProcessor::parent() const { return _parent; }

const TemplateProcessor::StreamState& TemplateProcessor::streamState() const {
  return _streamState;
}

const TemplateWaveform& TemplateProcessor::templateWaveform() const {
  return _crossCorrelation.templateWaveform();
}

std::size_t TemplateProcessor::size() const noexcept {
  return _stateMachines.size();
}

TemplateProcessor::Status TemplateProcessor::status() const { return _status; }

const Core::TimeSpan& TemplateProcessor::configuredBufferSize() const {
  return _buffer.configuredBufferSize();
}

void TemplateProcessor::setConfiguredBufferSize(
    const Core::TimeSpan& duration) {
  _buffer.setConfiguredBufferSize(duration);
}

bool TemplateProcessor::fill(processing::StreamState& streamState,
                             const Record* record, DoubleArrayPtr& data) {
  return _buffer.feed(record->timeWindow(), data);
}

TemplateProcessor::EventHandler::EventHandler(TemplateProcessor* processor)
    : processor{processor} {}

void TemplateProcessor::EventHandler::operator()(event::Record&& ev) {
  if (!processor->feed(ev.record.get())) {
    processor->setStatus(Status::kError);
  }
}

void TemplateProcessor::EventHandler::operator()(InternalEvent&& ev) {
  boost::variant2::visit(InternalEventHandler{processor}, std::move(ev));
}

TemplateProcessor::InternalEventHandler::InternalEventHandler(
    TemplateProcessor* processor)
    : processor{processor} {}

/* void operator()(event::Resample& ev); */

void TemplateProcessor::InternalEventHandler::operator()(event::Filter&& ev) {
  assert(processor);
  ev.filter = processor->_streamState.filter.get();

  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(
    event::CrossCorrelate&& ev) {
  assert(processor);
  ev.filter = &processor->_crossCorrelation;
  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(event::Process&& ev) {
  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(event::Finished&& ev) {
  if (!processor->_streamState.initialized) {
    processor->_streamState.initialized = ev.initialized;
  }

  boost::variant2::visit(HandleFinished{processor},
                         processor->_stateMachines.front().state());
}

TemplateProcessor::HandleFinished::HandleFinished(TemplateProcessor* processor)
    : processor{processor} {}

void TemplateProcessor::HandleFinished::operator()(
    const template_processor::SaturatedState& s) {
  assert(processor);
  processor->setStatus(Status::kDataClipped);
}

void TemplateProcessor::HandleFinished::operator()(
    const template_processor::CrossCorrelatedState& s) {
  assert(processor);
  processor->_stateMachines.pop();
  processor->tryToRunNextStateMachine();
}

void TemplateProcessor::HandleFinished::operator()(
    const template_processor::ProcessedState& s) {
  assert(processor);
  processor->_stateMachines.pop();
  processor->tryToRunNextStateMachine();
}

void TemplateProcessor::reset(StreamState& streamState) {
  std::unique_ptr<Filter> tmp{std::move(streamState.filter)};

  streamState = StreamState{};
  if (tmp) {
    streamState.filter.reset(tmp->clone());
  }
}

void TemplateProcessor::setStatus(Status status) { _status = status; }

bool TemplateProcessor::feed(const Record* record) {
  if (status() > Status::kWaitingForData ||
      !static_cast<bool>(record->data())) {
    return false;
  }

  DoubleArrayPtr data{
      dynamic_cast<DoubleArray*>(record->data()->copy(Array::DOUBLE))};

  if (_streamState.lastRecord) {
    if (record == _streamState.lastRecord) {
      return false;
    } else if (record->samplingFrequency() != _streamState.samplingFrequency) {
      SCDETECT_LOG_WARNING_PROCESSOR(
          this,
          "%s: sampling frequency changed, resetting stream (sfreq_record != "
          "sfreq_stream): %f != %f",
          record->streamID().c_str(), record->samplingFrequency(),
          _streamState.samplingFrequency);

      // flush buffer and buffer this record with the new sampling frequency
      flushBuffer();
      reset(_streamState);
      fill(_streamState, record, data);
    } else if (!handleGap(_streamState, record, data)) {
      return false;
    }

    _streamState.dataTimeWindow.setEndTime(record->endTime());
  }

  if (!_streamState.lastRecord) {
    try {
      setupStream(record);
    } catch (std::exception& e) {
      SCDETECT_LOG_WARNING_PROCESSOR(this, "%s: Failed to setup stream: %s",
                                     record->streamID().c_str(), e.what());
      return false;
    }
  }

  _streamState.lastSample = (*data)[data->size() - 1];

  fill(_streamState, record, data);
  if (_buffer.full()) {
    flushBuffer();
  }

  tryToRunNextStateMachine();

  // TODO(damb):
  // - feed to resampler; note that resampling cannot be treated as a separate
  // event, however, it may be offloaded to an executor

  return true;
}

void TemplateProcessor::flushBuffer() {
  auto bufferedRecord{_buffer.contiguousRecord<double>(
      _streamState.lastRecord->networkCode(),
      _streamState.lastRecord->stationCode(),
      _streamState.lastRecord->locationCode(),
      _streamState.lastRecord->channelCode(), _streamState.samplingFrequency)};
  store(bufferedRecord.release());
  _buffer.reset();
}

bool TemplateProcessor::store(const Record* record) {
  // create new state machine
  _stateMachines.emplace(record, this);
  return true;
}

bool TemplateProcessor::tryToRunNextStateMachine() {
  if (_stateMachines.empty()) {
    // nothing to do
    return false;
  }

  // send initial event to state machine
  auto& stateMachine{_stateMachines.front()};

  bool initState{0 == stateMachine.state().index()};
  if (initState) {
    if (_saturationThreshold) {
      event::CheckSaturation ev;
      ev.threshold = *_saturationThreshold;
      stateMachine.dispatch(ev);
    } else if (_streamState.filter) {
      event::Filter ev;
      ev.filter = _streamState.filter.get();
      stateMachine.dispatch(ev);
    } else {
      event::CrossCorrelate ev;
      ev.filter = &_crossCorrelation;
      stateMachine.dispatch(ev);
    }
    return true;
  }
  return false;
}

void TemplateProcessor::setupStream(const Record* record) {
  const auto& f{record->samplingFrequency()};
  _streamState.samplingFrequency = f;

  if (gapInterpolation()) {
    setMinimumGapThreshold(_streamState, record, id());
  }

  _streamState.neededSamples = std::lround(_initTime.value_or(0) * f);
  if (_streamState.filter) {
    _streamState.filter->setSamplingFrequency(f);
  }

  // update the received data timewindow
  _streamState.dataTimeWindow = record->timeWindow();

  if (_streamState.filter) {
    _streamState.filter->setStartTime(record->startTime());
    _streamState.filter->setStreamID(
        record->networkCode(), record->stationCode(), record->locationCode(),
        record->channelCode());
  }
}

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
