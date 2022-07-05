#ifndef SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_STATEMACHINE_H_
#define SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_STATEMACHINE_H_

#include <seiscomp/core/record.h>
#include <seiscomp/core/typedarray.h>

#include <boost/optional/optional.hpp>
#include <boost/variant2/variant.hpp>
#include <cstddef>
#include <memory>

#include "../event/template_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

class TemplateProcessor;

namespace template_processor {

struct InitState {};
struct SaturationCheckedState {};
struct SaturatedState {};
/* struct ResampledState {}; */
struct FilteredState {};
struct CrossCorrelatedState {};
struct ProcessedState {};

// Finite state machine implementing template waveform processing
class StateMachine {
 public:
  StateMachine(const Record* record, const TemplateProcessor* parent = nullptr);

  using Event = boost::variant2::variant<
      event::CheckSaturation, /*event::Resample,*/ event::Filter,
      event::CrossCorrelate, event::Process, event::Reset, event::Finished>;

  using State = boost::variant2::variant<
      InitState, SaturationCheckedState, SaturatedState,
      /*ResampledState,*/ FilteredState, CrossCorrelatedState, ProcessedState>;
  struct Transitions {
    explicit Transitions(StateMachine* stateMachine);
    /* boost::optional<State> operator()(InitState& s, const event::Resample&
     * ev); */
    boost::optional<State> operator()(InitState& s,
                                      const event::CheckSaturation& ev);
    boost::optional<State> operator()(InitState& s, const event::Filter& ev);
    boost::optional<State> operator()(InitState& s,
                                      const event::CrossCorrelate& ev);
    /* boost::optional<State> operator()(ResampledState& s, */
    /*                                   const event::CrossCorrelate& ev); */
    boost::optional<State> operator()(SaturationCheckedState& s,
                                      const event::CrossCorrelate& ev);
    boost::optional<State> operator()(FilteredState& s,
                                      const event::CrossCorrelate& ev);
    boost::optional<State> operator()(CrossCorrelatedState& s,
                                      const event::Process& ev);
    template <typename TState>
    boost::optional<StateMachine::State> operator()(
        TState& state, const event::Reset& ev) const {
      stateMachine->reset();
      return boost::optional<State>{InitState{}};
    }

    template <typename TState, typename TEvent>
    boost::optional<StateMachine::State> operator()(TState& state,
                                                    const TEvent& ev) const {
      return boost::none;
    }

    StateMachine* stateMachine;
  };
  friend Transitions;

  void dispatch(const Event& ev);

  // Reset the state machine to `InitState`
  void reset();

  // Returns whether the state machine has reached a final state
  bool finished();

  // Returns the current state
  const State& state() const;

  // Returns the record the state machine is associated with
  const Record& record() const;

  // Returns a pointer to the parent template processor or `nullptr` if the
  // state machine is not assigned to a template processor.
  const TemplateProcessor* parent() const;

 private:
  // Prepares `ev`
  void prepareEvent(Event& ev);

  // Filter the data and emits the subsequent events
  boost::optional<State> filterAndEmit(const event::Filter& ev);
  // Cross-correlates the data and emits subsequent events
  boost::optional<State> crossCorrelateAndEmit(const event::CrossCorrelate& ev);

  struct LocalMaxima {
    struct Value {
      double coefficient;
      size_t lagIdx;
    };

    using Values = std::vector<Value>;
    Values values;

    double prevCoefficient{-1};
    bool notDecreasing{false};

    void feed(double coefficient, std::size_t lagIdx);
  };

  // The current state
  State _state;

  // The record the processor state machine is associated with
  RecordCPtr _record;
  // The data the processor state machine is processing
  std::unique_ptr<DoubleArray> _data;

  const TemplateProcessor* _parent{nullptr};
};

}  // namespace template_processor
}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_STATEMACHINE_H_
