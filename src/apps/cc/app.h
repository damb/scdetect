#ifndef SCDETECT_APPS_CC_APP_H_
#define SCDETECT_APPS_CC_APP_H_

#include <seiscomp/client/application.h>
#include <seiscomp/client/streamapplication.h>
#include <seiscomp/core/datetime.h>
#include <seiscomp/core/record.h>
#include <seiscomp/datamodel/amplitude.h>
#include <seiscomp/datamodel/arrival.h>
#include <seiscomp/datamodel/databasequery.h>
#include <seiscomp/datamodel/eventparameters.h>
#include <seiscomp/datamodel/magnitude.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/pick.h>
#include <seiscomp/datamodel/stationmagnitude.h>
#include <seiscomp/system/commandline.h>

#include <boost/optional/optional.hpp>
#include <cassert>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "binding.h"
#include "config/detector.h"
//#include "config/template_family.h"
#include "detector/detector.h"
#include "exception.h"
#include "settings.h"
#include "util/waveform_stream_id.h"
#include "waveform.h"
#include "worker/detector_worker.h"

namespace Seiscomp {
namespace detect {

class Application : public Client::StreamApplication {
 public:
  Application(int argc, char **argv);

  class BaseException : public Exception {
   public:
    using Exception::Exception;
    BaseException();
  };

  class ConfigError : public BaseException {
   public:
    using BaseException::BaseException;
    ConfigError();
  };

  class DuplicatePublicObjectId : public BaseException {
   public:
    using BaseException::BaseException;
    DuplicatePublicObjectId();
  };

  struct Config {
    Config();

    void init(const Client::Application *app);
    void init(const System::CommandLine &commandline);

    std::string pathFilesystemCache;
    std::string urlEventDb;

    bool templatesPrepare{false};
    bool templatesNoCache{false};
    // Global flag indicating whether to enable `true` or disable `false`
    // calculating amplitudes (regardless of the configuration provided on
    // detector configuration level granularity).
    boost::optional<bool> amplitudesForceMode;
    // Global flag indicating whether to enable `true` or disable `false`
    // calculating magnitudes (regardless of the configuration provided on
    // detector configuration level granularity.
    boost::optional<bool> magnitudesForceMode;

    // Flag with forces the waveform buffer size
    boost::optional<Core::TimeSpan> forcedWaveformBufferSize{
        Core::TimeSpan{300.0}};

    // Defines if a detector should be initialized although template
    // processors could not be initialized due to missing waveform data.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipTemplateIfNoWaveformData{true};
    // Defines if a detector should be initialized although template processors
    // could not be initialized due to missing stream information in the
    // inventory.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipTemplateIfNoStreamData{true};
    // Defines if a detector should be initialized although template processors
    // could not be initialized due to missing sensor location information in
    // the inventory.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipTemplateIfNoSensorLocationData{true};
    // Defines if a detector should be initialized although template processors
    // could not be initialized due to a missing pick in the event parameters.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipTemplateIfNoPick{true};
    // Defines if a template family should be initialized despite reference
    // configurations could not be initialized due to missing waveform data.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipReferenceConfigIfNoWaveformData{true};
    // Defines if a template family should be initialized although reference
    // configurations could not be initialized due to missing stream
    // information in the inventory.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipReferenceConfigIfNoStreamData{true};
    // Defines if a template family should be initialized although reference
    // configurations could not be initialized due to missing sensor location
    // information in the inventory.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipReferenceConfigIfNoSensorLocationData{true};
    // Defines if a template family should be initialized although reference
    // configurations could not be initialized due to a missing pick in the
    // event parameters.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipReferenceConfigIfNoPick{true};
    // Defines if a template family should be initialized although reference
    // configurations could not be initialized due to missing bindings
    // configuration.
    // XXX(damb): For the time being, this configuration parameter is not
    // provided to module users.
    bool skipReferenceConfigIfNoBindings{true};

    // Input
    std::string pathTemplateJson;
    std::string pathTemplateFamilyJson;

    // Reprocessing / playback
    struct {
      std::string startTimeStr;
      std::string endTimeStr;

      Core::Time startTime;
      Core::Time endTime;

      // Indicates if playback mode is enabled/disabled
      bool enabled{false};
    } playbackConfig;

    // Messaging
    bool offlineMode{false};
    bool noPublish{false};
    std::string pathEp;

    std::string amplitudeMessagingGroup{"AMPLITUDE"};

    // default configurations
    config::PublishConfig publishConfig;

    config::DetectorConfig detectorConfig;

    config::StreamConfig streamConfig;

    /* config::TemplateFamilyConfig::ReferenceConfig::SensorLocationConfig */
    /*     templateFamilySensorLocationConfig; */

    // binding default configurations
    binding::SensorLocationConfig sensorLocationBindings;
  };

  const char *version() override;

 protected:
  void createCommandLineDescription() override;
  bool validateParameters() override;
  bool initConfiguration() override;
  bool handleCommandLineOptions() override;

  bool init() override;
  bool run() override;
  void done() override;

  void handleRecord(Record *rec) override;

 private:
  using WorkerId = std::string;
  using WaveformStreamId = std::string;
  using DetectorWorker = worker::DetectorWorker;
  using DetectorWorkers = std::vector<std::shared_ptr<DetectorWorker>>;
  using DetectorWorkerIdx = std::unordered_map<WorkerId, std::size_t>;
  using Detector = DetectorWorker::Detector;

  struct DetectionItem {
    explicit DetectionItem(const DataModel::OriginPtr &origin)
        : origin{origin} {
      assert(origin);
    }

    Core::Time expired{Core::Time::GMT() +
                       Core::TimeSpan{10 * 60.0 /*seconds*/}};

    struct ProcessorConfig {
      bool gapInterpolation;
      Core::TimeSpan gapThreshold;
      Core::TimeSpan gapTolerance;
    };

    ProcessorConfig config;

    using ProcessorId = std::string;
    using Amplitudes = std::unordered_map<ProcessorId, DataModel::AmplitudePtr>;
    Amplitudes amplitudes;
    using Magnitudes = std::vector<DataModel::StationMagnitudePtr>;
    Magnitudes magnitudes;
    using NetworkMagnitudes = std::vector<DataModel::MagnitudePtr>;
    NetworkMagnitudes networkMagnitudes;

    struct ArrivalPick {
      DataModel::ArrivalPtr arrival;
      DataModel::PickPtr pick;
    };
    using ArrivalPicks = std::vector<ArrivalPick>;
    // Picks and arrivals which are associated to the detection (i.e. both
    // detected picks and *template picks*)
    ArrivalPicks arrivalPicks;

    using WaveformStreamId = std::string;
    struct Pick {
      // The authorative full waveform stream identifier
      WaveformStreamId authorativeWaveformStreamId;
      DataModel::PickCPtr pick;
    };
    using AmplitudePickMap = std::unordered_map<ProcessorId, Pick>;
    // Picks used for amplitude calculation
    AmplitudePickMap amplitudePickMap;

    DataModel::OriginPtr origin;

    std::string detectorId;
    // std::shared_ptr<const detector::Detector::Detection> detection;

    std::size_t numberOfRequiredAmplitudes{};
    std::size_t numberOfRequiredMagnitudes{};

    bool published{false};

    const std::string &id() const { return origin->publicID(); }

    bool amplitudesReady() const {
      std::size_t count{};
      for (const auto &amplitudePair : amplitudes) {
        if (amplitudePair.second) {
          ++count;
        }
      }
      return numberOfRequiredAmplitudes == count;
    }
    bool magnitudesReady() const {
      return numberOfRequiredMagnitudes == magnitudes.size();
    }
    bool ready() const {
      return (amplitudesReady() && magnitudesReady()) ||
             (Core::Time::GMT() >= expired);
    }

    friend bool operator==(const DetectionItem &lhs, const DetectionItem &rhs) {
      return lhs.id() == rhs.id();
    }
    friend bool operator!=(const DetectionItem &lhs, const DetectionItem &rhs) {
      return !(lhs == rhs);
    }
  };

  bool isEventDatabaseEnabled() const;

  // Load events either from `eventDb` or `db`
  bool loadEvents(const std::string &eventDb, DataModel::DatabaseQueryPtr db);

  // Initializes `DetectorWorker`s
  //
  // - Returns `true` if at least a single worker was initialized, else `false`
  bool initDetectorWorkers(std::ifstream &ifs,
                           WaveformHandlerIface *waveformHandler);

  // Creates detectors
  //
  // - `ifs` references a template configuration input file stream
  std::vector<std::unique_ptr<Detector>> createDetectors(
      std::ifstream &ifs, WaveformHandlerIface *waveformHandler);

  bool startDetectorWorkerThreads();
  void shutdownDetectorWorkers();

  Config _config;
  binding::Bindings _bindings;

  ObjectLog *_outputOrigins;
  ObjectLog *_outputAmplitudes;

  DataModel::EventParametersPtr _ep;

  DetectorWorkers _detectorWorkers;
  DetectorWorkerIdx _detectorWorkerIdx;

  std::vector<std::thread> _detectorWorkerThreads;
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_APP_H_
