#ifndef SCDETECT_APPS_CC_DETECTOR_DETECTOR_H_
#define SCDETECT_APPS_CC_DETECTOR_DETECTOR_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/defs.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/datamodel/arrival.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/pick.h>
#include <seiscomp/datamodel/sensorlocation.h>

#include <boost/optional/optional.hpp>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "../builder.h"
#include "../config/detector.h"
#include "../processing/processor.h"
#include "../waveform.h"
#include "arrival.h"
#include "detail.h"
#include "detection.h"
#include "detection_candidate.h"
#include "detection_candidate_processor.h"
#include "detection_processor.h"
#include "event/link.h"
#include "event/record.h"
#include "event/status.h"
#include "exception.h"
#include "inventory.h"
#include "linker.h"
#include "linker/association.h"
#include "match_result.h"
#include "template_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {
namespace detail {

template <typename T>
struct Identity {
  using type = T;
};

}  // namespace detail

// Event-driven detector implementation. It does neither implement an event
// loop nor an event queue. Instead, it relies on e.g. `worker::DetectorWorker`
// which implements these facilities.
class Detector : public processing::Processor {
  using TemplateProcessors = std::vector<TemplateProcessor>;

 public:
  using const_iterator = TemplateProcessors::const_iterator;
  using size_type = std::size_t;
  using Event = boost::variant2::variant<event::Link, event::Status,
                                         TemplateProcessor::Event>;
  using Detection = detector::Detection;
  using EmitDetectionCallback =
      std::function<void(const Detector *, std::unique_ptr<Detection>)>;
  using EmitEventCallback = std::function<void(Event &&)>;

  enum class Status {
    kWaitingForData = 0,
    kClosed,
    kTerminated,
    kError,
  };

  class Builder : public detect::Builder<Detector> {
   public:
    explicit Builder(const std::string &originId);

    Builder &setId(const std::string &id);

    Builder &setConfig(const config::PublishConfig &publishConfig,
                       const config::DetectorConfig &detectorConfig,
                       bool playback);

    // Set stream related template configuration where `streamId` refers to the
    // waveform stream identifier of the stream to be processed.
    Builder &setStream(const std::string &streamId,
                       const config::StreamConfig &streamConfig,
                       WaveformHandlerIface *waveformHandler);

   protected:
    void finalize() override;

   private:
    struct TemplateProcessorConfig {
      // Template processor
      TemplateProcessor processor;
      // `TemplateWaveformProcessor` specific merging threshold
      boost::optional<double> mergingThreshold;

      struct MetaData {
        // The template's sensor location associated
        DataModel::SensorLocationCPtr sensorLocation;
        // The template related pick
        DataModel::PickCPtr pick;
        // The template related arrival
        DataModel::ArrivalCPtr arrival;
      } metadata;
    };

    using TemplateProcessorConfigs =
        std::unordered_map<std::string, TemplateProcessorConfig>;

    static bool isValidArrival(const DataModel::Arrival &arrival,
                               const DataModel::Pick &pick);

    void setTemplateOrigin();

    void setMergingStrategy(const std::string &strategyId);

    TemplateProcessorConfigs _processorConfigs;

    DetectionProcessor _detectionProcessor;

    DataModel::OriginCPtr _origin;
  };

  friend class Builder;
  static Builder Create(const std::string &originId);

  const_iterator begin() const noexcept;
  const_iterator end() const noexcept;
  const_iterator cbegin() const noexcept;
  const_iterator cend() const noexcept;

  // Returns the number of underlying `TemplateProcessor`s
  std::size_t size() const noexcept;
  // Returns whether the detector references underlying `TemplateProcessor`s
  bool empty() const noexcept;

  // Enables (`true`) or disables (`false`) the detector
  void enable(bool enable);
  bool enabled() const;

  // Returns the detector's status
  Status status() const;
  // Closes the detector
  //
  // - the detector won't accept new waveform data (i.e. `event::Record`s),
  // anymore
  void close();
  // Returns whether the detector is finished
  bool finished() const;
  // Terminates the detector
  void terminate();

  // Dispatches `ev`
  void dispatch(Event &&ev);

  // Posts `ev` by means of calling the callback previously configured by means
  // of `setEmitEventCallback()`.
  void postEvent(Event &&ev) const;

  // Resets the detector
  void reset();
  // Flushes pending detections
  void flush();

  // Returns a reference to the underlying `TemplateProcessor` or `nullptr` if
  // there is no underlying `TemplateProcessor` identified by `processorId`.
  const TemplateProcessor *processor(const std::string &processorId) const;

  // Returns a reference to the detector configuration
  const config::DetectorConfig &detectorConfig() const;

  void setEmitDetectionCallback(EmitDetectionCallback callback);

  void setEmitEventCallback(EmitEventCallback callback);

  // Returns the waveform stream identifiers the detector is associated with
  std::set<std::string> associatedWaveformStreamIds() const;

  // Returns a reference to the underlying linker
  const Linker &linker() const;

  // Sets the maximum data latency to be allowed
  void setMaxLatency(boost::optional<Core::TimeSpan> latency = boost::none);
  // Returns the configured maximum allowed data latency or `boost::none` if
  // the data latency is not evaluated
  const boost::optional<Core::TimeSpan> &maxLatency() const;

 private:
  struct EventHandler {
    using InternalEvent = TemplateProcessor::Event;
    explicit EventHandler(Detector *detector);

    void operator()(event::Link &&ev);
    void operator()(InternalEvent &&ev);

    template <typename TEvent>
    void operator()(TEvent &&ev) {}

    Detector *detector{nullptr};
  };

  friend EventHandler;

  class InternalEventHandler {
   public:
    explicit InternalEventHandler(Detector *detector);
    template <typename TInternalEvent>
    void operator()(TInternalEvent &&ev) {
      dispatch(std::forward<TInternalEvent>(ev),
               detail::Identity<TInternalEvent>{});
    }

    Detector *detector{nullptr};

   private:
    template <typename TInternalEvent>
    void dispatch(TInternalEvent &&ev, detail::Identity<TInternalEvent>) {
      const auto &templateProcessorId{
          boost::variant2::visit(TemplateIdExtractor{}, ev)};

      // XXX(damb): no bounds checking
      detector
          ->_templateProcessors
              [detector->_templateProcessorIdIdx[templateProcessorId]]
          .dispatch(std::forward<TInternalEvent>(ev));
    }

    void dispatch(event::Record &&ev, detail::Identity<event::Record>) {
      if (detector->status() == Status::kClosed) {
        throw BaseException{"processor closed"};
      }
      detector->dispatch(std::move(ev));
    }
  };

  friend InternalEventHandler;

  struct TemplateIdExtractor {
    template <typename TInternalEvent>
    std::string operator()(const TInternalEvent &ev) {
      return ev.templateProcessorId;
    }
  };

  Detector();

  // Registers the `templateProcessor`. Records fed to the detector are
  // associated with the underlying `templateProcessor` by the waveform stream
  // identifier `waveformStreamId`.
  // Besides, `templateProcessor` is associated together with the template
  // arrival `arrival` and `sensorLocation`.
  void registerTemplateProcessor(
      TemplateProcessor &&templateProcessor,
      /* const boost::optional<Core::TimeSpan> &templateProcessorBufferSize, */
      const std::string &waveformStreamId, const Arrival &arrival,
      const SensorLocation &sensorLocation,
      const boost::optional<double> &mergingThreshold);

  // Returns whether `record` has an acceptable latency
  bool hasAcceptableLatency(const Record *record) const;

  void onTriggeredCallback(
      const DetectionCandidateProcessor *detectionCandidateProcessor,
      const DetectionCandidate &candidate);

  void onLinkerResultCallback(linker::Association &&association);

  void link(const detail::ProcessorIdType &templateProcessorId,
            MatchResult &&matchResult);
  void dispatch(event::Record &&ev);

  void resetTemplateProcessorLinkingInfo();

  TemplateProcessors _templateProcessors;
  using TemplateProcessorWaveformStreamIdIdx =
      std::unordered_map<detail::WaveformStreamIdType, std::size_t>;
  TemplateProcessorWaveformStreamIdIdx _templateProcessorWaveformStreamIdIdx;
  using TemplateProcessorIdIdx =
      std::unordered_map<detail::ProcessorIdType, std::size_t>;
  TemplateProcessorIdIdx _templateProcessorIdIdx;

  std::vector<bool> _templateProcessorLinkingInfo;

  // The linker used for pick association
  Linker _linker;

  DetectionCandidateProcessor _detectionCandidateProcessor;

  config::DetectorConfig _config;

  EmitEventCallback _emitEventCallback;

  // The maximum acceptable data latency
  boost::optional<Core::TimeSpan> _maxLatency;

  Status _status{Status::kWaitingForData};
  bool _enabled{true};
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_DETECTOR_H_
