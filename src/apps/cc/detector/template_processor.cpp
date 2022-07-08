#include "template_processor.h"

#include <boost/variant2/variant.hpp>
#include <memory>

#include "../def.h"
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

void TemplateProcessor::dispatch(Event& ev) {
  boost::variant2::visit(EventHandler{this}, ev);
}

void TemplateProcessor::setFilter(
    std::unique_ptr<Filter> filter,
    const boost::optional<Core::TimeSpan>& initTime) {
  _streamState.filter = std::move(filter);
  _initTime = std::max(initTime.value_or(0.0),
                       templateWaveform().configuredEndTime() -
                           templateWaveform().configuredStartTime());
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

TemplateProcessor::EventHandler::EventHandler(TemplateProcessor* processor)
    : processor{processor} {}

void TemplateProcessor::EventHandler::operator()(const event::Record& ev) {
  if (processor->status() > Status::kWaitingForData ||
      !static_cast<bool>(ev.record->data())) {
    return;
  }

  DoubleArrayPtr data{
      dynamic_cast<DoubleArray*>(ev.record->data()->copy(Array::DOUBLE))};

  auto& streamState{processor->_streamState};
  if (streamState.lastRecord) {
    if (ev.record == streamState.lastRecord) {
      return;
    } else if (ev.record->samplingFrequency() !=
               streamState.samplingFrequency) {
      SCDETECT_LOG_WARNING_PROCESSOR(
          processor,
          "%s: sampling frequency changed, resetting stream (sfreq_record != "
          "sfreq_stream): %f != %f",
          ev.record->streamID().c_str(), ev.record->samplingFrequency(),
          streamState.samplingFrequency);

      // flush buffer and buffer this record with the new sampling frequency
      flushBuffer(streamState);
      processor->_buffer.feed(ev.record->timeWindow(), data);
      reset(streamState);
    } else if (!processor->handleGap(streamState, ev.record.get(), data)) {
      return;
    }

    streamState.dataTimeWindow.setEndTime(ev.record->endTime());
  }

  if (!streamState.lastRecord) {
    try {
      processor->setupStream(ev.record.get());
    } catch (std::exception& e) {
      SCDETECT_LOG_WARNING_PROCESSOR(processor,
                                     "%s: Failed to setup stream: %s",
                                     ev.record->streamID().c_str(), e.what());
      return;
    }
  }

  streamState.lastSample = (*data)[data->size() - 1];

  processor->_buffer.feed(ev.record->timeWindow(), data);
  if (processor->_buffer.full()) {
    flushBuffer(streamState);
  }

  processor->tryToRunNextStateMachine();

  // TODO(damb):
  // - feed to resampler; note that resampling cannot be treated as a separate
  // event, however, it may be offloaded to an executor
}

void TemplateProcessor::EventHandler::operator()(InternalEvent& ev) {
  boost::variant2::visit(InternalEventHandler{processor}, ev);
}

void TemplateProcessor::EventHandler::flushBuffer(
    const StreamState& streamState) {
  auto bufferedRecord{processor->_buffer.contiguousRecord<double>(
      streamState.lastRecord->networkCode(),
      streamState.lastRecord->stationCode(),
      streamState.lastRecord->locationCode(),
      streamState.lastRecord->channelCode(), streamState.samplingFrequency)};
  processor->store(bufferedRecord.release());
  processor->_buffer.reset();
}

TemplateProcessor::InternalEventHandler::InternalEventHandler(
    TemplateProcessor* processor)
    : processor{processor} {}

/* void operator()(event::Resample& ev); */

void TemplateProcessor::InternalEventHandler::operator()(event::Filter& ev) {
  assert(processor);
  ev.filter = processor->_streamState.filter.get();

  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(
    event::CrossCorrelate& ev) {
  assert(processor);
  ev.filter = &processor->_crossCorrelation;
  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(event::Process& ev) {
  processor->_stateMachines.front().dispatch(ev);
}

void TemplateProcessor::InternalEventHandler::operator()(event::Finished& ev) {
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
