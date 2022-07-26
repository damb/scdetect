#ifndef SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_H_
#define SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/record.h>

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <queue>

#include "../def.h"
#include "../processing/detail/gap_interpolate.h"
#include "../processing/processor.h"
#include "../processing/stream.h"
#include "../template_waveform.h"
#include "detail.h"
#include "event/record.h"
#include "event/status.h"
#include "template_processor/buffer.h"
#include "template_processor/state_machine.h"

namespace Seiscomp {
namespace detect {
namespace detector {

class Detector;

// Non-blocking event-driven template waveform processor implementation
//
// - Creates for every record fed a dedicated `ProcessorStateMachine`
// - Implements gap interpolation facilities
class TemplateProcessor : public processing::Processor,
                          public processing::detail::InterpolateGaps {
 public:
  using Filter = DoubleFilter;
  using InternalEvent = template_processor::StateMachine::Event;
  using Event = boost::variant2::variant<event::Record, InternalEvent>;

  enum class Status {
    kWaitingForData = 0,
    kClosed,
    kTerminated,
    // No associated value yet (error code?)
    kError,
    // Indicates saturated/clipped data
    kDataClipped,
  };

  // Describes the current state of a stream
  struct StreamState : public processing::StreamState {
    // The filter (if used)
    std::unique_ptr<Filter> filter;

    // Number of samples required to finish initialization
    std::size_t neededSamples{0};
    // Number of samples already received
    std::size_t receivedSamples{0};
    // Initialization state
    bool initialized{false};
  };

  explicit TemplateProcessor(TemplateWaveform templateWaveform,
                             Detector* parent = nullptr);

  // Returns the processor's current stream state
  const StreamState& streamState() const;

  // Closes the processor
  //
  // - after closing the processor does not accept new waveform data (i.e.
  // `event::Record`s) anymore
  void close();
  // Flushes the processor
  void flush();
  // Resets the processor
  void reset();
  // Returns whether the processor has finished
  bool finished() const;

  void dispatch(Event&& ev);

  // Sets `filter` with the corresponding filter `initTime`
  void setFilter(std::unique_ptr<DoubleFilter> filter,
                 const boost::optional<Core::TimeSpan>& initTime = boost::none);
  // Returns the processor's initialization time
  boost::optional<Core::TimeSpan> initTime() const;

  // Returns a pointer to the parent detector or `nullptr` in case the
  // processor is not assigned to a detector.
  const Detector* parent() const;
  // Returns the underlying template waveform
  const TemplateWaveform& templateWaveform() const;

  // Returns the number of `StateMachine`s currently queued
  std::size_t size() const noexcept;

  // Returns the processor's status
  Status status() const;

  // Sets the target sampling frequency
  //
  // - if the sampling frequency changes, the processor is implicitly reset
  void setTargetSamplingFrequency(boost::optional<double> freq);
  boost::optional<double> targetSamplingFrequency() const;

  const Core::TimeSpan& configuredBufferSize() const;
  void setConfiguredBufferSize(const Core::TimeSpan& duration);

 protected:
  bool fill(processing::StreamState& streamState, const Record* record,
            DoubleArrayPtr& data) override;

 private:
  struct EventHandler {
    explicit EventHandler(TemplateProcessor* processor);
    void operator()(event::Record&& ev);
    void operator()(InternalEvent&& ev);

    TemplateProcessor* processor{nullptr};
  };

  friend EventHandler;

  struct InternalEventHandler {
    explicit InternalEventHandler(TemplateProcessor* processor);
    /* void operator()(event::Resample& ev); */
    void operator()(event::Filter&& ev);
    void operator()(event::CrossCorrelate&& ev);
    void operator()(event::Process&& ev);
    void operator()(event::Finished&& ev);

    template <typename TEvent>
    void operator()(TEvent&& ev) {}

    TemplateProcessor* processor{nullptr};
  };

  friend InternalEventHandler;

  // Handles `event::Finished` regarding `StateMachine::Status`
  struct HandleFinished {
    explicit HandleFinished(TemplateProcessor* processor);
    void operator()(const template_processor::SaturatedState& s);
    void operator()(const template_processor::CrossCorrelatedState& s);
    void operator()(const template_processor::ProcessedState& s);

    template <typename TState>
    void operator()(const TState& s) {
      assert(processor);
      processor->setStatus(Status::kError);
    }

    TemplateProcessor* processor{nullptr};
  };

  friend HandleFinished;

  using StateMachines = std::queue<template_processor::StateMachine>;

  static void reset(StreamState& streamState);

  static event::Status createStatusEvent(
      const TemplateProcessor* templateProcessor, event::Status::Type type);

  // Set the processor's status
  void setStatus(Status status);

  // Feeds `record` to the processor
  bool feed(const Record* record);

  void flushBuffer();

  // Creates a new `StateMachine` for each record fed
  bool store(const Record* record);

  void setupStream(const Record* record);

  // Tries to run the next state machine. Returns `true` if a state machine was
  // run, else `false`.
  bool tryToRunNextStateMachine();

  // XXX(damb): for optimization: in future, state machines might be executed
  // in parallel. However, the `TemplateProcessor` needs to make sure that
  // transitions are executed sequentially (i.e. due to the nature of the the
  // time series data itself, filters, etc.).
  StateMachines _stateMachines;

  detail::CrossCorrelation _crossCorrelation;

  // Waveform data record buffer
  template_processor::Buffer _buffer;

  StreamState _streamState;

  boost::optional<double> _saturationThreshold;
  // Optional target sampling frequency the processor is operating on
  boost::optional<double> _targetSamplingFrequency;

  // Processor initialization time (usually corresponds to the filter
  // initialization time)
  boost::optional<Core::TimeSpan> _initTime;

  Status _status{Status::kWaitingForData};

  // Reference to the parent detector
  Detector* _parent{nullptr};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_TEMPLATEPROCESSOR_H_
