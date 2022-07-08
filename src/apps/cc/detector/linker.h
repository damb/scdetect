#ifndef SCDETECT_APPS_CC_DETECTOR_LINKER_H_
#define SCDETECT_APPS_CC_DETECTOR_LINKER_H_

#include <seiscomp/core/datetime.h>
#include <seiscomp/core/timewindow.h>

#include <boost/optional/optional.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "arrival.h"
#include "detail.h"
#include "linker/association.h"
#include "linker/pot.h"
#include "template_processor.h"

namespace Seiscomp {
namespace detect {
namespace detector {

// A linker which implements matrix based phase association
class Linker {
 public:
  using PublishResultCallback = std::function<void(linker::Association &&)>;
  using MergingStrategy = std::function<bool(
      const linker::Association::TemplateResult &, double, double)>;

  static const Core::TimeSpan DefaultSafetyMargin;

  explicit Linker(const Core::TimeSpan &onHold = Core::TimeSpan{0.0},
                  const Core::TimeSpan &arrivalOffsetThres = Core::TimeSpan{
                      2.0e-6});

  // Sets the arrival offset threshold
  void setArrivalOffsetThreshold(const boost::optional<Core::TimeSpan> &thres);
  // Returns the current arrival offset threshold
  boost::optional<Core::TimeSpan> arrivalOffsetThreshold() const;
  // Sets the association threshold
  void setAssociationThreshold(const boost::optional<double> &thres);
  // Returns the association threshold
  boost::optional<double> associationThreshold() const;
  // Configures the linker with a minimum number of required arrivals before
  // issuing a result
  void setMinArrivals(const boost::optional<std::size_t> &n);
  // Returns the minimum number of arrivals required for linking
  boost::optional<std::size_t> minArrivals() const;
  // Sets the *on hold* duration
  void setOnHold(const Core::TimeSpan &duration);
  // Returns the current *on hold* duration
  Core::TimeSpan onHold() const;

  // Sets the linker's merging strategy based on `mergingStrategyTypeId`
  void setMergingStrategy(MergingStrategy mergingStrategy);
  // Returns the number of associated channels
  std::size_t channelCount() const;
  // Returns the number of associated processors
  std::size_t size() const noexcept;

  // Register the template processor identified by `templateProcessorId` and
  // associated with the template arrival `arrival` for linking.
  //
  // - optionally set a template processor specific `mergingThreshold`
  void registerTemplateProcessor(
      const detail::ProcessorIdType &templateProcessorId,
      const Arrival &arrival, const boost::optional<double> &mergingThreshold);

  // Remove the template processor identified by `templateProcessorId`
  void unregisterTemplateProcessor(const std::string &templateProcessorId);
  // Reset the linker
  //
  // - drops all pending results
  void reset();
  // Flushes the linker
  void flush();

  // Feeds the `templateProcessor`'s result `matchResult` to the linker
  void feed(const TemplateProcessor &templateProcessor,
            MatchResult &&matchResult);

  // Set the publish callback function
  void setResultCallback(PublishResultCallback callback);

 private:
  // Creates a POT where all participating processors are enabled
  void createPot();

  struct Candidate;
  struct CandidatePOTData {
    std::vector<double> offsets;
    std::vector<bool> mask;

    CandidatePOTData() = default;
    explicit CandidatePOTData(std::size_t n)
        : offsets(n, linker::POT::tableDefault), mask(n, false) {}
  };

  // `TemplateProcessor` metadata required for phase association
  struct TemplateProcessorInfo {
    // The template arrival associated
    Arrival arrival;
    // The processor specific merging threshold
    boost::optional<double> mergingThreshold;
  };

  using TemplateProcessorInfos =
      std::unordered_map<detail::ProcessorIdType, TemplateProcessorInfo>;
  TemplateProcessorInfos _templateProcessorInfos;

  struct Candidate {
    // The final association
    linker::Association association;
    // The time after the event is considered as expired
    Core::Time expired;

    explicit Candidate(const Core::Time &expired);
    // Feeds the template result `res` to the candidate in order to be merged
    void feed(const detail::ProcessorIdType &templateProcessorId,
              const linker::Association::TemplateResult &res);
    // Returns the number of associated processors
    std::size_t associatedProcessorCount() const noexcept;
    // Returns `true` if the event must be considered as expired
    bool isExpired(const Core::Time &now) const;
  };

  using CandidateQueue = std::list<Candidate>;

  // Processes the result `result` from `templateProcessor`
  void process(const TemplateProcessor &templateProcessor,
               const linker::Association::TemplateResult &result);
  // Emit a result
  void emitResult(linker::Association &&result);

  CandidatePOTData createCandidatePOTData(
      const Candidate &candidate, const detail::ProcessorIdType &processorId,
      const linker::Association::TemplateResult &newResult);

  CandidateQueue _queue;

  // The linker's reference POT
  linker::POT _pot;
  bool _potValid{false};

  // The arrival offset threshold; if `boost::none` arrival offset threshold
  // validation is disabled; the default arrival offset corresponds to twice
  // the maximum accuracy `scdetect` is operating when it comes to trimming
  // waveforms (1 micro second (i.e. 1 us)).
  boost::optional<Core::TimeSpan> _thresArrivalOffset{2.0e-6};
  // The association threshold indicating when template results are taken into
  // consideration
  boost::optional<double> _thresAssociation;
  // The minimum number of arrivals required in order to issue a result
  boost::optional<size_t> _minArrivals;

  // The maximum time events are placed on hold before either being emitted or
  // dropped
  Core::TimeSpan _onHold{0.0};

  // The merging strategy used while linking
  MergingStrategy _mergingStrategy{
      [](const linker::Association::TemplateResult &result,
         double associationThreshold, double mergingThreshold) {
        return result.resultIt->coefficient >= associationThreshold;
      }};

  // The result callback function
  PublishResultCallback _resultCallback;
};

}  // namespace detector
}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_DETECTOR_LINKER_H_
