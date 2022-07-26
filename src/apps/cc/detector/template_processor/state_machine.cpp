#include "state_machine.h"

#include <cmath>

#include "../detector.h"
#include "../match_result.h"
#include "../template_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace template_processor {

StateMachine::StateMachine(const Record* record,
                           const TemplateProcessor* parent)
    : _record{record},
      _data{dynamic_cast<DoubleArray*>(_record->data()->copy(Array::DOUBLE))},
      _parent{parent} {}

StateMachine::Transitions::Transitions(StateMachine* stateMachine)
    : stateMachine{stateMachine} {}

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    InitState& s, const event::CheckSaturation& ev) {
  assert(stateMachine);
  assert(stateMachine->_data);

  const auto* samples{stateMachine->_data->typedData()};
  for (int i = 0; i < stateMachine->_data->size(); ++i) {
    if (fabs(samples[i]) >= ev.threshold) {
      event::Finished finished;
      finished.initialized = stateMachine->parent()->streamState().initialized;
      StateMachine::Event nextEvent{finished};
      stateMachine->prepareEvent(nextEvent);
      stateMachine->parent()->parent()->postEvent(std::move(nextEvent));
      return boost::optional<State>{SaturatedState{}};
    }
  }

  StateMachine::Event nextEvent;
  if (stateMachine->parent()->streamState().filter) {
    nextEvent = event::Filter{};
  } else {
    nextEvent = event::CrossCorrelate{};
  }
  stateMachine->prepareEvent(nextEvent);
  stateMachine->parent()->parent()->postEvent(std::move(nextEvent));
  return boost::optional<State>{SaturationCheckedState{}};
}

/* boost::optional<StateMachine::State> */
/* StateMachine::Transitions::operator()(InitState&
 * s, */
/*                                                const event::Resample& ev) {}
 */

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    InitState& s, const event::Filter& ev) {
  assert(stateMachine);
  return stateMachine->filterAndEmit(ev);
}

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    InitState& s, const event::CrossCorrelate& ev) {
  assert(stateMachine);
  return stateMachine->crossCorrelateAndEmit(ev);
}

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    SaturationCheckedState& s, const event::CrossCorrelate& ev) {
  assert(stateMachine);
  return stateMachine->crossCorrelateAndEmit(ev);
}

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    FilteredState& s, const event::CrossCorrelate& ev) {
  assert(stateMachine);
  return stateMachine->crossCorrelateAndEmit(ev);
}

boost::optional<StateMachine::State> StateMachine::Transitions::operator()(
    CrossCorrelatedState& s, const event::Process& ev) {
  assert(stateMachine);
  assert(stateMachine->_data);
  assert(stateMachine->parent());

  const auto& record{*(stateMachine->_record)};
  const auto& data{*(stateMachine->_data.get())};

  const auto n{static_cast<size_t>(data.size())};
  int startIdx{0};
  Core::Time start{record.timeWindow().startTime()};
  // check if processing start lies within the record
  if (!stateMachine->parent()->streamState().initialized) {
    startIdx = std::max(
        0, static_cast<int>(n) -
               static_cast<int>(
                   stateMachine->parent()->streamState().receivedSamples -
                   stateMachine->parent()->streamState().neededSamples));
    const auto t{static_cast<double>(startIdx) / n};
    start =
        record.startTime() + Core::TimeSpan{record.timeWindow().length() * t};
  }

  LocalMaxima localMaxima;
  for (auto i{static_cast<size_t>(startIdx)}; i < n; ++i) {
    localMaxima.feed(data[i], i);
  }

  MatchResult matchResult;
  if (localMaxima.values.empty()) {
    // nothing to link
    event::Finished finished;
    finished.initialized = stateMachine->parent()->streamState().initialized;
    StateMachine::Event nextEvent{finished};
    stateMachine->prepareEvent(nextEvent);
    stateMachine->parent()->parent()->postEvent(std::move(nextEvent));
  } else {
    const Core::TimeWindow tw{start, stateMachine->record().endTime()};
    for (const auto& m : localMaxima.values) {
      // take cross-correlation filter delay into account i.e. the template
      // processor's result is referring to a time window shifted to the past
      const auto matchIdx{static_cast<int>(
          m.lagIdx - stateMachine->parent()->templateWaveform().size() + 1)};
      const auto t{static_cast<double>(matchIdx) / n};

      matchResult.localMaxima.push_back(
          MatchResult::Value{Core::TimeSpan{tw.length() * t}, m.coefficient});
    }

    matchResult.timeWindow = tw;

    event::Link nextEvent;
    nextEvent.detectorId = stateMachine->parent()->parent()->id();
    nextEvent.templateProcessorId = stateMachine->parent()->id();
    nextEvent.matchResult = std::move(matchResult);
    stateMachine->parent()->parent()->postEvent(std::move(nextEvent));
  }

  return boost::optional<State>{ProcessedState{}};
}

void StateMachine::dispatch(const Event& ev) {
  auto nextState{boost::variant2::visit(Transitions{this}, _state, ev)};
  if (nextState) {
    _state = *nextState;
  }
}

void StateMachine::reset() {
  _data.reset(dynamic_cast<DoubleArray*>(_record->data()->copy(Array::DOUBLE)));
}

const StateMachine::State& StateMachine::state() const { return _state; }

const Record& StateMachine::record() const { return *_record; }

const TemplateProcessor* StateMachine::parent() const { return _parent; }

void StateMachine::prepareEvent(Event& ev) {
  assert(parent());
  assert(parent()->parent());

  boost::variant2::visit(
      [this](auto& e) {
        e.detectorId = parent()->parent()->id();
        e.templateProcessorId = parent()->id();
      },
      ev);
}

boost::optional<StateMachine::State> StateMachine::filterAndEmit(
    const event::Filter& ev) {
  assert(_data);

  ev.filter->apply(*_data);

  // generate event for next processing step
  StateMachine::Event nextEvent{event::CrossCorrelate{}};
  prepareEvent(nextEvent);
  parent()->parent()->postEvent(std::move(nextEvent));
  return boost::optional<State>{FilteredState{}};
}

boost::optional<StateMachine::State> StateMachine::crossCorrelateAndEmit(
    const event::CrossCorrelate& ev) {
  assert(_data);

  // execute synchronously
  ev.filter->apply(*_data);

  StateMachine::Event nextEvent;
  if (!parent()->streamState().initialized) {
    if (parent()->streamState().neededSamples >=
        parent()->streamState().receivedSamples) {
      nextEvent = event::Process{};
    } else {
      nextEvent = event::Finished{};
    }
  } else {
    nextEvent = event::Process{};
  }
  prepareEvent(nextEvent);
  parent()->parent()->postEvent(std::move(nextEvent));

  // XXX(damb): actually, the state is *cross-correlating* when returning
  return boost::optional<State>{CrossCorrelatedState{}};
}

void StateMachine::LocalMaxima::feed(double coefficient, std::size_t lagIdx) {
  if (!std::isfinite(coefficient)) {
    return;
  }

  if (coefficient < prevCoefficient && notDecreasing) {
    values.push_back({prevCoefficient, --lagIdx});
  }

  notDecreasing = coefficient >= prevCoefficient;
  prevCoefficient = coefficient;
}

}  // namespace template_processor
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp
