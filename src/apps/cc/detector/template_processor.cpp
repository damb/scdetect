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
    : _crossCorrelation{std::move(templateWaveform)},

      _parent{parent} {}

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

TemplateProcessor::EventHandler::EventHandler(TemplateProcessor* processor)
    : processor{processor} {}

void TemplateProcessor::EventHandler::operator()(event::Record& ev) {
  // TODO TODO TODO
  // - buffer record
  // - create a new state machine (if buffer full)
}

void TemplateProcessor::EventHandler::operator()(InternalEvent& ev) {
  boost::variant2::visit(InternalEventHandler{processor}, ev);
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
  // TODO TODO TODO
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

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
