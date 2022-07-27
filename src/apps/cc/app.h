#ifndef SCDETECT_APPS_CC_APP_H_
#define SCDETECT_APPS_CC_APP_H_

#include <seiscomp/client/streamapplication.h>
#include <seiscomp/core/baseobject.h>
#include <seiscomp/datamodel/eventparameters.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/pick.h>

#include <boost/optional/optional.hpp>
#include <cassert>
#include <cstddef>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "binding.h"
#include "config/detector.h"
//#include "config/template_family.h"
#include "detection.h"
#include "detector/detector.h"
#include "exception.h"
#include "notification/detection.h"
#include "settings.h"
#include "util/waveform_stream_id.h"
#include "waveform.h"
#include "worker/detector_worker.h"
#include "worker/event/command.h"

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

  void exit(int returnCode) override;

 protected:
  void createCommandLineDescription() override;
  bool validateParameters() override;
  bool initConfiguration() override;
  bool handleCommandLineOptions() override;

  bool init() override;
  bool run() override;
  void done() override;

  void handleInterrupt(int sig) override;

  // Dispatches custom worker notifications
  bool dispatchNotification(int type, Core::BaseObject *obj) override;

  void handleRecord(Record *rec) override;

 private:
  using WorkerId = std::string;
  using WaveformStreamId = std::string;
  using DetectorWorker = worker::DetectorWorker;
  using DetectorWorkers =
      std::unordered_map<DetectorWorker::Id, std::shared_ptr<DetectorWorker>>;
  using DetectorWorkerThreads =
      std::unordered_map<DetectorWorker::Id, std::thread>;
  using Detector = DetectorWorker::Detector;
  using WorkerCommand = worker::event::Command;

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

  void flushDetectorWorker(const DetectorWorker::Id &workerId);
  void shutdownDetectorWorker(const DetectorWorker::Id &workerId);

  void processDetection(const notification::Detection &detectionNotification);
  bool prepareDetection(const notification::Detection &detectionNotification,
                        Detection &detection);
  void publishDetection(const Detection &detection);

  Config _config;
  binding::Bindings _bindings;

  ObjectLog *_outputOrigins;
  ObjectLog *_outputAmplitudes;

  DataModel::EventParametersPtr _ep;

  DetectorWorkers _detectorWorkers;
  DetectorWorkerThreads _detectorWorkerThreads;
};

}  // namespace detect
}  // namespace Seiscomp

#endif  // SCDETECT_APPS_CC_APP_H_
