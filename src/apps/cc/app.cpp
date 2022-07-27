#include "app.h"

#include <seiscomp/client/streamapplication.h>
#include <seiscomp/core/arrayfactory.h>
#include <seiscomp/core/exceptions.h>
#include <seiscomp/core/record.h>
#include <seiscomp/core/strings.h>
#include <seiscomp/core/timewindow.h>
#include <seiscomp/datamodel/comment.h>
#include <seiscomp/datamodel/magnitude.h>
#include <seiscomp/datamodel/notifier.h>
#include <seiscomp/datamodel/origin.h>
#include <seiscomp/datamodel/phase.h>
#include <seiscomp/datamodel/realquantity.h>
#include <seiscomp/datamodel/stationmagnitudecontribution.h>
#include <seiscomp/datamodel/timequantity.h>
#include <seiscomp/datamodel/timewindow.h>
#include <seiscomp/datamodel/waveformstreamid.h>
#include <seiscomp/io/archive/xmlarchive.h>
#include <seiscomp/io/recordinput.h>
#include <seiscomp/math/geo.h>
#include <seiscomp/processing/stream.h>
#include <seiscomp/utils/files.h>

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <exception>
#include <ios>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "builder.h"
#include "config/detector.h"
#include "config/exception.h"
#include "config/validators.h"
#include "detector/arrival.h"
#include "detector/detector.h"
#include "eventstore.h"
#include "log.h"
#include "notification.h"
#include "notification/detection.h"
#include "processing/processor.h"
#include "resamplerstore.h"
#include "seiscomp/datamodel/arrival.h"
#include "seiscomp/datamodel/creationinfo.h"
#include "seiscomp/datamodel/pick.h"
#include "util/memory.h"
#include "util/util.h"
#include "util/waveform_stream_id.h"
#include "version.h"
#include "worker/detector_worker.h"
#include "worker/event/command.h"
#include "worker/recordstream.h"

namespace Seiscomp {
namespace detect {

namespace {

DataModel::PickPtr createPick(const detector::Arrival &arrival,
                              bool asTemplateArrivalPick,
                              const boost::optional<DataModel::CreationInfo>
                                  &creationInfo = boost::none) {
  DataModel::PickPtr ret{DataModel::Pick::Create()};
  if (!ret) {
    throw Application::DuplicatePublicObjectId{"duplicate pick identifier"};
  }

  ret->setTime(DataModel::TimeQuantity{arrival.pick.time, boost::none,
                                       arrival.pick.lowerUncertainty,
                                       arrival.pick.upperUncertainty});
  util::WaveformStreamID waveformStreamId{arrival.pick.waveformStreamId};
  if (asTemplateArrivalPick) {
    ret->setWaveformID(DataModel::WaveformStreamID{
        waveformStreamId.netCode(), waveformStreamId.staCode(),
        waveformStreamId.locCode(), waveformStreamId.chaCode(), ""});
  } else {
    ret->setWaveformID(DataModel::WaveformStreamID{
        waveformStreamId.netCode(), waveformStreamId.staCode(),
        waveformStreamId.locCode(),
        util::getBandAndSourceCode(waveformStreamId), ""});
  }
  ret->setEvaluationMode(DataModel::EvaluationMode(DataModel::AUTOMATIC));

  if (arrival.pick.phaseHint) {
    ret->setPhaseHint(DataModel::Phase{*arrival.pick.phaseHint});
  }

  ret->setCreationInfo(creationInfo);

  return ret;
};

DataModel::ArrivalPtr createArrival(
    const detector::Arrival &arrival, const DataModel::Pick &pick,
    const boost::optional<DataModel::CreationInfo> &creationInfo =
        boost::none) {
  auto ret{util::make_smart<DataModel::Arrival>()};
  if (!ret) {
    throw Application::DuplicatePublicObjectId{"duplicate arrival identifier"};
  }
  ret->setCreationInfo(creationInfo);
  ret->setPickID(pick.publicID());
  ret->setPhase(arrival.phase);
  if (arrival.weight.value_or(0) != 0) {
    ret->setWeight(arrival.weight);
  }

  return ret;
}

DataModel::CommentPtr createTemplateWaveformTimeInfoComment(
    const detector::Detection::ContributionInfo &contributionInfo) {
  auto ret{util::make_smart<DataModel::Comment>()};
  ret->setId(settings::kTemplateWaveformTimeInfoPickCommentId);
  ret->setText(contributionInfo.templateWaveformStartTime.iso() +
               settings::kTemplateWaveformTimeInfoPickCommentIdSep +
               contributionInfo.templateWaveformEndTime.iso() +
               settings::kTemplateWaveformTimeInfoPickCommentIdSep +
               contributionInfo.templateWaveformReferenceTime.iso());

  return ret;
}

}  // namespace

Application::Application(int argc, char **argv)
    : StreamApplication(argc, argv) {
  setLoadStationsEnabled(true);
  setLoadInventoryEnabled(true);
  setLoadConfigModuleEnabled(true);
  setMessagingEnabled(true);
  setAutoAcquisitionStart(false);

  setPrimaryMessagingGroup("LOCATION");
}

Application::BaseException::BaseException()
    : Exception{"base application exception"} {}

Application::ConfigError::ConfigError()
    : BaseException{"application configuration error"} {}

Application::DuplicatePublicObjectId::DuplicatePublicObjectId()
    : BaseException{"duplicate public object identifier"} {}

const char *Application::version() { return kVersion; }

void Application::exit(int returnCode) {
  _returnCode = returnCode;

  for (auto &pair : _detectorWorkers) {
    pair.second->postEvent(WorkerCommand{WorkerCommand::Type::kTerminate});
  }
}

void Application::createCommandLineDescription() {
  StreamApplication::createCommandLineDescription();

  commandline().addOption(
      "Database", "event-db",
      "load events from the given database or file, format: "
      "[service://]location",
      &_config.urlEventDb);

  commandline().addOption(
      "Records", "record-starttime",
      "defines a start time (YYYY-MM-DDTHH:MM:SS formatted) for "
      "requesting records from the configured archive recordstream; "
      "implicitly enables reprocessing/playback mode",
      &_config.playbackConfig.startTimeStr);
  commandline().addOption(
      "Records", "record-endtime",
      "defines an end time (YYYY-MM-DDTHH:MM:SS formatted) for "
      "requesting records from the configured archive recordstream; "
      "implicitly enables reprocessing/playback mode",
      &_config.playbackConfig.endTimeStr);

  commandline().addGroup("Mode");
  commandline().addOption(
      "Mode", "offline",
      "offline mode, do not connect to the messaging system (implies "
      "--no-publish i.e. no objects are sent)");
  commandline().addOption("Mode", "no-publish", "do not send any objects");
  commandline().addOption(
      "Mode", "ep",
      "same as --no-publish, but outputs all event parameters scml "
      "formatted; specifying the output path as '-' (a single dash) will "
      "force the output to be redirected to stdout",
      &_config.pathEp);
  commandline().addOption(
      "Mode", "playback",
      "Use playback mode that does not restrict the maximum allowed "
      "data latency");
  commandline().addOption(
      "Mode", "templates-prepare",
      "load template waveform data from the configured recordstream "
      "and save it in the module's caching directory, then exit");
  commandline().addOption(
      "Mode", "templates-reload",
      "force reloading template waveform data and omit cached waveform data");
  commandline().addOption(
      "Mode", "amplitudes-force",
      "enables/disables the calculation of amplitudes regardless of the "
      "configuration provided on detector configuration level granularity",
      &_config.amplitudesForceMode, false);
  commandline().addOption(
      "Mode", "magnitudes-force",
      "enables/disables the calculation of magnitudes regardless of the "
      "configuration provided on detector configuration level granularity",
      &_config.magnitudesForceMode, false);

  commandline().addGroup("Input");
  commandline().addOption(
      "Input", "templates-json",
      "path to a template configuration file (json-formatted)",
      &_config.pathTemplateJson);
  commandline().addOption(
      "Input", "templates-family-json",
      "path to a template family configuration file (json-formatted)",
      &_config.pathTemplateFamilyJson);
}

bool Application::validateParameters() {
  if (!StreamApplication::validateParameters()) return false;

  // validate paths
  if (!_config.pathTemplateJson.empty() &&
      !Util::fileExists(_config.pathTemplateJson)) {
    SCDETECT_LOG_ERROR("Invalid path to template configuration file: %s",
                       _config.pathTemplateJson.c_str());
    return false;
  }
  if (!_config.pathTemplateFamilyJson.empty() &&
      !Util::fileExists(_config.pathTemplateFamilyJson)) {
    SCDETECT_LOG_ERROR("Invalid path to template family configuration file: %s",
                       _config.pathTemplateFamilyJson.c_str());
    return false;
  }

  // validate reprocessing config
  auto validateAndStoreTime = [](const std::string &timeStr,
                                 Core::Time &result) {
    if (!timeStr.empty() && !result.fromString(timeStr.c_str(), "%FT%T")) {
      SCDETECT_LOG_ERROR("Invalid time: %s", timeStr.c_str());
      return false;
    }
    return true;
  };

  if (!validateAndStoreTime(_config.playbackConfig.startTimeStr,
                            _config.playbackConfig.startTime)) {
    return false;
  }
  if (!validateAndStoreTime(_config.playbackConfig.endTimeStr,
                            _config.playbackConfig.endTime)) {
    return false;
  }

  if (!config::validateXCorrThreshold(_config.detectorConfig.triggerOn)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'triggerOnThreshold': %f. Not in "
        "interval [-1,1].",
        _config.detectorConfig.triggerOn);
    return false;
  }
  if (!config::validateXCorrThreshold(_config.detectorConfig.triggerOff)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'triggerOffThreshold': %f. Not in "
        "interval [-1,1].",
        _config.detectorConfig.triggerOff);
    return false;
  }

  if (_config.streamConfig.filter && !(*_config.streamConfig.filter).empty()) {
    std::string err;
    if (!config::validateFilter(*_config.streamConfig.filter, err)) {
      SCDETECT_LOG_ERROR("Invalid configuration: 'filter': %s (%s)",
                         (*_config.streamConfig.filter).c_str(), err.c_str());
      return false;
    }
  }
  if (!util::isGeZero(_config.streamConfig.initTime)) {
    SCDETECT_LOG_ERROR("Invalid configuration: 'initTime': %f. Must be >= 0.",
                       _config.streamConfig.initTime);
    return false;
  }

  if (_config.forcedWaveformBufferSize &&
      !util::isGeZero(*_config.forcedWaveformBufferSize)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'waveformBufferSize': %f. Must be >= 0.",
        _config.streamConfig.initTime);
    return false;
  }

  if (!config::validateArrivalOffsetThreshold(
          _config.detectorConfig.arrivalOffsetThreshold)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'arrivalOffsetThreshold': %f. "
        "Must be < 0 or >= 2.0e-6",
        _config.detectorConfig.arrivalOffsetThreshold);
    return false;
  }
  if (!config::validateMinArrivals(_config.detectorConfig.minArrivals)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'minimumArrivals': %d. "
        "Must be < 0 or >= 1",
        _config.detectorConfig.minArrivals);
    return false;
  }
  if (!config::validateLinkerMergingStrategy(
          _config.detectorConfig.mergingStrategy)) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'mergingStrategy': %s. Must be one of: {%s}",
        _config.detectorConfig.mergingStrategy.c_str(),
        boost::algorithm::join(config::kValidLinkerMergingStrategies, ",")
            .c_str());

    return false;
  }
  if (_config.streamConfig.templateConfig.wfStart >=
      _config.streamConfig.templateConfig.wfEnd) {
    SCDETECT_LOG_ERROR(
        "Invalid configuration: 'waveformStart' >= 'waveformEnd' : %f >= %f",
        _config.streamConfig.templateConfig.wfStart,
        _config.streamConfig.templateConfig.wfEnd);
    return false;
  }

  return true;
}

bool Application::handleCommandLineOptions() {
  _config.init(commandline());

  if (_config.offlineMode) {
    SCDETECT_LOG_INFO("Disable messaging");
    setMessagingEnabled(false);

    _config.noPublish = true;
  }

  if (!_config.noPublish && commandline().hasOption("ep")) {
    _config.noPublish = true;
  }

  bool magnitudesForcedEnabled{_config.magnitudesForceMode &&
                               *_config.magnitudesForceMode};
  bool amplitudesForcedDisabled{_config.amplitudesForceMode &&
                                !*_config.amplitudesForceMode &&
                                !magnitudesForcedEnabled};

  // disable config module if required
  if (amplitudesForcedDisabled) {
    setLoadConfigModuleEnabled(false);
  }
  // disable the database if required
  if (!isInventoryDatabaseEnabled() && !isEventDatabaseEnabled() &&
      (amplitudesForcedDisabled || !isConfigDatabaseEnabled())) {
    SCDETECT_LOG_INFO("Disable database connection");
    setDatabaseEnabled(false, false);
  }

  return false;
}

bool Application::initConfiguration() {
  if (!StreamApplication::initConfiguration()) return false;

  try {
    _config.init(this);
  } catch (ValueException &e) {
    SCDETECT_LOG_ERROR("Failed to initialize configuration: %s", e.what());
    return false;
  }

  return true;
}

bool Application::init() {
  if (!StreamApplication::init()) return false;

  _outputOrigins = addOutputObjectLog("origin", primaryMessagingGroup());
  _outputAmplitudes =
      addOutputObjectLog("amplitude", _config.amplitudeMessagingGroup);

  if (_config.playbackConfig.enabled) {
    SCDETECT_LOG_INFO("Playback mode enabled");
  }

  // load event related data
  if (!loadEvents(_config.urlEventDb, query())) {
    SCDETECT_LOG_ERROR("Failed to load events");
    return false;
  }

  // TODO(damb): Check if std::unique_ptr wouldn't be sufficient, here.
  WaveformHandlerIfacePtr waveformHandler{
      util::make_smart<WaveformHandler>(recordStreamURL())};
  if (!_config.templatesNoCache) {
    // cache template waveforms on filesystem
    _config.pathFilesystemCache =
        boost::filesystem::path(_config.pathFilesystemCache).string();
    if (!Util::pathExists(_config.pathFilesystemCache) &&
        !Util::createPath(_config.pathFilesystemCache)) {
      SCDETECT_LOG_ERROR("Failed to create path (waveform cache): %s",
                         _config.pathFilesystemCache.c_str());
      return false;
    }

    waveformHandler = util::make_smart<FileSystemCache>(
        waveformHandler, _config.pathFilesystemCache,
        settings::kCacheRawWaveforms);
  }
  // cache demeaned template waveform snippets in order to speed up the
  // initialization procedure
  waveformHandler =
      util::make_smart<InMemoryCache>(waveformHandler, /*raw=*/false);

  if (_config.pathTemplateJson.empty()) {
    SCDETECT_LOG_ERROR("Missing template configuration file.");
    return false;
  }

  SCDETECT_LOG_INFO("Loading template configuration from: \"%s\"",
                    _config.pathTemplateJson.c_str());
  try {
    std::ifstream ifs{_config.pathTemplateJson};

    if (!initDetectorWorkers(ifs, waveformHandler.get())) {
      return false;
    }
  } catch (std::ifstream::failure &e) {
    SCDETECT_LOG_ERROR("Failed to parse template configuration (%s): %s",
                       _config.pathTemplateJson.c_str(), e.what());
    return false;
  }

  // load bindings
  if (configModule()) {
    _bindings.setDefault(_config.sensorLocationBindings);

    SCDETECT_LOG_DEBUG("Loading bindings configuration");
    _bindings.load(&configuration(), configModule(), name());
  }

  // free memory after initialization
  EventStore::Instance().reset();

  return true;
}

bool Application::run() {
  SCDETECT_LOG_DEBUG("Application initialized");

  if (_config.templatesPrepare) {
    SCDETECT_LOG_DEBUG(
        "Requested application exit after template initialization");
    return true;
  }

  if (commandline().hasOption("ep")) {
    _ep = util::make_smart<DataModel::EventParameters>();
  }

  if (!startDetectorWorkerThreads()) {
    return false;
  }

  return StreamApplication::run();
}

void Application::done() {
  if (!_config.templatesPrepare) {
    shutdownDetectorWorkers();

    // TODO(damb): flush pending detections

    if (_ep) {
      IO::XMLArchive ar;
      ar.create(_config.pathEp.empty() ? "-" : _config.pathEp.c_str());
      ar.setFormattedOutput(true);
      ar << _ep;
      ar.close();
      SCDETECT_LOG_DEBUG("Found %lu origins", _ep->originCount());
      _ep.reset();
    }
  }

  EventStore::Instance().reset();
  RecordResamplerStore::Instance().reset();

  StreamApplication::done();
}

void Application::handleInterrupt(int sig) {
  switch (sig) {
    // XXX(damb): the corresponding signal handler for signal `SIGINT` is
    // installed by `system::Application`
    case SIGINT: {
      exit(_returnCode);
      break;
    }
    default:
      SCDETECT_LOG_DEBUG("Unhandled signal received");
  }
}

bool Application::dispatchNotification(int type, Core::BaseObject *obj) {
  using WorkerNotificationType = WorkerNotification::Type;
  if (type < static_cast<int>(WorkerNotificationType::kDetection)) {
    assert(obj);

    WorkerNotificationCPtr workerNotification{
        static_cast<WorkerNotification *>(obj)};

    const auto &workerId{workerNotification->workerId};

    std::ostringstream oss;
    oss << workerNotification->threadId;
    const auto tag{workerId + " (" + oss.str() + ")"};

    switch (type) {
      case static_cast<int>(WorkerNotificationType::kInitializing):
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker initializing ...");
        break;
      case static_cast<int>(WorkerNotificationType::kInitialized):
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker initialized");
        break;
      case static_cast<int>(WorkerNotificationType::kRunning):
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker running ...");
        break;
      case static_cast<int>(WorkerNotificationType::kFinishedRecordStreaming): {
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker finished record streaming");
        // close worker
        auto &worker{_detectorWorkers[workerNotification->workerId]};
        worker->postEvent(WorkerCommand{WorkerCommand::Type::kClose});
        break;
      }
      case static_cast<int>(WorkerNotificationType::kFinishedProcessing): {
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker finished processing");
        // shutdown worker
        auto &worker{_detectorWorkers[workerNotification->workerId]};
        worker->postEvent(WorkerCommand{WorkerCommand::Type::kShutdown});
        break;
      }
      case static_cast<int>(WorkerNotificationType::kShuttingDown):
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker shutting down ...");
        break;
      case static_cast<int>(WorkerNotificationType::kShutdown): {
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker shutdown");

        auto allDetectorWorkersFinished{std::all_of(
            std::begin(_detectorWorkers), std::end(_detectorWorkers),
            [](const DetectorWorkers::value_type &p) {
              return p.second->status() == DetectorWorker::Status::kFinished;
            })};

        if (allDetectorWorkersFinished) {
          _exitRequested = true;
        }

        break;
      }
      case static_cast<int>(WorkerNotificationType::kTerminated): {
        SCDETECT_LOG_DEBUG_TAGGED(tag, "Worker terminated");

        auto allDetectorWorkersTerminated{std::all_of(
            std::begin(_detectorWorkers), std::end(_detectorWorkers),
            [](const DetectorWorkers::value_type &p) {
              return p.second->status() == DetectorWorker::Status::kTerminated;
            })};

        if (allDetectorWorkersTerminated) {
          Client::StreamApplication::exit(_returnCode);
        }

        break;
      }
      default:
        SCDETECT_LOG_WARNING("Unknown worker notification type");
        return false;
    }

    return true;
  }

  using DetectionNotification = notification::Detection;
  using DetectionNotificationCPtr = notification::DetectionCPtr;
  DetectionNotificationCPtr detectionNotification{
      static_cast<DetectionNotification *>(obj)};

  assert(detectionNotification->detection);
  const auto &detectorId{detectionNotification->detectorId};
  const auto &detection{detectionNotification->detection};

  SCDETECT_LOG_DEBUG_TAGGED(
      detectorId, "Processing detection (time=%s, associated_results=%d) ...",
      detection->origin.time.iso().c_str(),
      detection->contributionInfos.size());

  processDetection(*detectionNotification);

  return true;
}

void Application::handleRecord(Record *rec) {
  // XXX(damb): the ownership of `rec` is transferred.
  RecordPtr ownershipGuard{rec};
  // XXX(damb): do nothing
}

bool Application::isEventDatabaseEnabled() const {
  return _config.urlEventDb.empty();
}

bool Application::loadEvents(const std::string &eventDb,
                             DataModel::DatabaseQueryPtr db) {
  bool loaded{false};
  if (!eventDb.empty()) {
    SCDETECT_LOG_INFO("Loading events from %s", eventDb.c_str());

    auto loadFromFile = [this, &loaded](const std::string &path) {
      try {
        EventStore::Instance().load(path);
        loaded = true;
      } catch (const std::exception &e) {
        auto msg{Core::stringify("Failed to load events: %s", e.what())};
        if (isDatabaseEnabled()) {
          SCDETECT_LOG_WARNING("%s", msg.c_str());
        } else {
          SCDETECT_LOG_ERROR("%s", msg.c_str());
        }
      }
    };

    if (eventDb.find("://") == std::string::npos) {
      loadFromFile(eventDb);
    } else if (eventDb.find("file://") == 0) {
      loadFromFile(eventDb.substr(7));
    } else {
      SCDETECT_LOG_INFO("Trying to connect to %s", eventDb.c_str());
      IO::DatabaseInterfacePtr db{IO::DatabaseInterface::Open(eventDb.c_str())};
      if (db) {
        SCDETECT_LOG_INFO("Connected successfully");
        auto query{util::make_smart<DataModel::DatabaseQuery>(db.get())};
        EventStore::Instance().load(query.get());
        loaded = true;
      } else {
        SCDETECT_LOG_WARNING("Database connection to %s failed",
                             eventDb.c_str());
      }
    }
  }

  if (!loaded && isDatabaseEnabled()) {
    SCDETECT_LOG_INFO("Loading events from %s", databaseURI().c_str());
    try {
      EventStore::Instance().load(query());
      loaded = true;
    } catch (std::exception &e) {
      SCDETECT_LOG_ERROR("Failed to load events: %s", e.what());
    }
  }

  if (loaded) {
    SCDETECT_LOG_INFO("Finished loading events");
  }

  return loaded;
}

bool Application::initDetectorWorkers(std::ifstream &ifs,
                                      WaveformHandlerIface *waveformHandler) {
  std::vector<std::unique_ptr<Detector>> detectors;
  try {
    detectors = createDetectors(ifs, waveformHandler);
  } catch (const ConfigError &e) {
    SCDETECT_LOG_ERROR("Failed to create detectors: %s", e.what());
    return false;
  }

  if (detectors.empty()) {
    SCDETECT_LOG_ERROR("Failed to create detectors: no detectors initialized.");
    return false;
  }

  // TODO(damb): perform load balancing; currently all detectors are handled
  // by a single `DetectorWorker`
  worker::RecordStream recordStream{recordStreamURL()};
  auto worker{std::make_shared<DetectorWorker>(std::move(recordStream),
                                               std::move(detectors))};
  worker->setId(util::createUUID());

  // configure notification callback
  worker->setEmitApplicationNotificationCallback(
      [this](const Client::Notification &n) { sendNotification(n); });

  _detectorWorkers.emplace(worker->id(), worker);

  return !_detectorWorkers.empty();
}

std::vector<std::unique_ptr<Application::Detector>>
Application::createDetectors(std::ifstream &ifs,
                             WaveformHandlerIface *waveformHandler) {
  std::vector<std::unique_ptr<Detector>> ret;
  try {
    boost::property_tree::ptree pt;
    boost::property_tree::read_json(ifs, pt);

    for (const auto &templateSettingPt : pt) {
      try {
        config::TemplateConfig tc{templateSettingPt.second,
                                  _config.detectorConfig, _config.streamConfig,
                                  _config.publishConfig};
        if (!config::hasUniqueTemplateIds(tc)) {
          throw ConfigError{"failed to initialize detector (id=" +
                            tc.detectorId() + "): template ids must be unique"};
        }

        SCDETECT_LOG_DEBUG("Creating detector (id=%s) ... ",
                           tc.detectorId().c_str());

        auto detectorBuilder{
            std::move(Detector::Create(tc.originId())
                          .setId(tc.detectorId())
                          .setConfig(tc.publishConfig(), tc.detectorConfig(),
                                     _config.playbackConfig.enabled))};

        std::vector<WaveformStreamId> waveformStreamIds;
        for (const auto &streamConfigPair : tc) {
          try {
            detectorBuilder.setStream(streamConfigPair.first,
                                      streamConfigPair.second, waveformHandler);
          } catch (builder::NoSensorLocation &e) {
            if (_config.skipTemplateIfNoSensorLocationData) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              std::string{e.what()}};
          } catch (builder::NoStream &e) {
            if (_config.skipTemplateIfNoStreamData) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              std::string{e.what()}};
          } catch (builder::NoPick &e) {
            if (_config.skipTemplateIfNoPick) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              std::string{e.what()}};
          } catch (builder::NoWaveformData &e) {
            if (_config.skipTemplateIfNoWaveformData) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              std::string{e.what()}};
          }
          waveformStreamIds.push_back(streamConfigPair.first);
        }

        auto detector{detectorBuilder.build()};
        if (detector->empty()) {
          SCDETECT_LOG_WARNING(
              "Failed to create detector: failed to create template "
              "processors");
          continue;
        }

        ret.emplace_back(std::move(detector));

      } catch (Exception &e) {
        SCDETECT_LOG_WARNING("Failed to create detector: %s. Skipping.",
                             e.what());
        continue;
      }
    }
  } catch (boost::property_tree::json_parser::json_parser_error &e) {
    throw ConfigError{"failed to parse template configuration (" +
                      _config.pathTemplateJson + "): " + e.what()};
  } catch (std::ifstream::failure &e) {
    throw ConfigError{"failed to parse template configuration (" +
                      _config.pathTemplateJson + "): " + e.what()};
  } catch (std::exception &e) {
    throw ConfigError{"failed to parse template configuration (" +
                      _config.pathTemplateJson + "): " + e.what()};
  } catch (...) {
    throw ConfigError{"failed to parse template configuration (" +
                      _config.pathTemplateJson + ")"};
  }
  return ret;
}

bool Application::startDetectorWorkerThreads() {
  for (auto &p : _detectorWorkers) {
    const auto &workerId{p.first};
    auto &worker{p.second};
    _detectorWorkerThreads.emplace(workerId,
                                   std::thread{[worker]() { worker->exec(); }});
  }

  return true;
}

void Application::shutdownDetectorWorkers() {
  for (auto &p : _detectorWorkerThreads) {
    auto &workerThread{p.second};
    if (workerThread.joinable()) {
      try {
        workerThread.join();
      } catch (const std::system_error &e) {
        SCDETECT_LOG_WARNING("Failed to shutdown worker: %s", e.what());
        continue;
      }
    }
  }
}

void Application::shutdownDetectorWorker(const DetectorWorker::Id &workerId) {
  auto &workerThread{_detectorWorkerThreads[workerId]};
  if (workerThread.joinable()) {
    try {
      workerThread.join();
    } catch (const std::system_error &e) {
      SCDETECT_LOG_WARNING("Failed to shutdown worker: %s", e.what());
    }
  }
}

void Application::processDetection(
    const notification::Detection &detectionNotification) {
  Detection prepared;
  if (!prepareDetection(detectionNotification, prepared)) {
    SCDETECT_LOG_WARNING_TAGGED(detectionNotification.detectorId,
                                "Failed to prepare detection");
    return;
  }
  publishDetection(prepared);
}

bool Application::prepareDetection(
    const notification::Detection &detectionNotification,
    Detection &detection) {
  const auto &d{detectionNotification.detection};
  const auto &detectorId{detectionNotification.detectorId};
  assert(d);

  Core::Time now{Core::Time::GMT()};

  DataModel::CreationInfo ci;
  ci.setAgencyID(agencyID());
  ci.setAuthor(author());
  ci.setCreationTime(now);

  DataModel::OriginPtr origin{DataModel::Origin::Create()};
  if (!origin) {
    SCDETECT_LOG_WARNING_TAGGED(detectorId,
                                "Internal error: duplicate origin identifier");
    return false;
  }

  {
    auto comment{util::make_smart<DataModel::Comment>()};
    comment->setId(settings::kDetectorIdCommentId);
    comment->setText(detectorId);
    origin->add(comment.get());
  }
  {
    auto comment{util::make_smart<DataModel::Comment>()};
    comment->setId("scdetectResultCCC");
    comment->setText(std::to_string(d->score));
    origin->add(comment.get());
  }

  origin->setCreationInfo(ci);
  origin->setLatitude(DataModel::RealQuantity(d->origin.latitude));
  origin->setLongitude(DataModel::RealQuantity(d->origin.longitude));
  if (d->origin.depth) {
    origin->setDepth(DataModel::RealQuantity(*(d->origin.depth)));
  }
  origin->setTime(DataModel::TimeQuantity(d->origin.time));
  origin->setMethodID(d->publishConfig.originMethodId);
  origin->setEpicenterFixed(true);
  origin->setEvaluationMode(DataModel::EvaluationMode(DataModel::AUTOMATIC));

  std::vector<double> azimuths;
  std::vector<double> distances;
  for (const auto &resultPair : d->contributionInfos) {
    double az, baz, dist;
    const auto &sensorLocation{resultPair.second.sensorLocation};
    Math::Geo::delazi(d->origin.latitude, d->origin.longitude,
                      sensorLocation.latitude, sensorLocation.longitude, &dist,
                      &az, &baz);

    distances.push_back(dist);
    azimuths.push_back(az);
  }

  std::sort(azimuths.begin(), azimuths.end());
  std::sort(distances.begin(), distances.end());

  DataModel::OriginQuality originQuality;
  if (azimuths.size() > 2) {
    double azGap{};
    for (size_t i = 0; i < azimuths.size() - 1; ++i)
      azGap = (azimuths[i + 1] - azimuths[i]) > azGap
                  ? (azimuths[i + 1] - azimuths[i])
                  : azGap;

    originQuality.setAzimuthalGap(azGap);
  }

  if (!distances.empty()) {
    originQuality.setMinimumDistance(distances.front());
    originQuality.setMaximumDistance(distances.back());
    originQuality.setMedianDistance(distances[distances.size() / 2]);
  }

  originQuality.setStandardError(1.0 - d->score);
  originQuality.setAssociatedStationCount(d->numStationsAssociated);
  originQuality.setUsedStationCount(d->numStationsUsed);
  originQuality.setAssociatedPhaseCount(d->numChannelsAssociated);
  originQuality.setUsedPhaseCount(d->numChannelsUsed);

  origin->setQuality(originQuality);

  detection.origin = origin;
  detection.detectorId = detectorId;

  auto createPicks{d->publishConfig.createArrivals};
  if (createPicks) {
    using PhaseCode = std::string;
    using ProcessedPhaseCodes = std::unordered_set<PhaseCode>;
    using SensorLocationStreamId = std::string;
    using SensorLocationStreamIdProcessedPhaseCodesMap =
        std::unordered_map<SensorLocationStreamId, ProcessedPhaseCodes>;
    SensorLocationStreamIdProcessedPhaseCodesMap processedPhaseCodes;

    for (const auto &contributionInfoPair : d->contributionInfos) {
      const auto &contributionInfo{contributionInfoPair.second};

      auto sensorLocationStreamId{
          util::getSensorLocationStreamId(util::WaveformStreamID{
              contributionInfo.arrival.pick.waveformStreamId})};

      try {
        const auto pick{createPick(contributionInfo.arrival, false, ci)};
        if (!pick->add(createTemplateWaveformTimeInfoComment(contributionInfo)
                           .get())) {
          SCDETECT_LOG_WARNING_TAGGED(detectorId,
                                      "Internal error: failed to add "
                                      "template waveform time info comment");
        }

        auto &processed{processedPhaseCodes[sensorLocationStreamId]};
        auto phaseAlreadyProcessed{
            std::find(std::begin(processed), std::end(processed),
                      contributionInfo.arrival.phase) != std::end(processed)};
        // XXX(damb): assign a phase only once per sensor location
        if (!phaseAlreadyProcessed) {
          const auto arrival{
              createArrival(contributionInfo.arrival, *pick, ci)};
          detection.arrivalPicks.push_back({arrival, pick});
          processed.emplace(contributionInfo.arrival.phase);
        }
      } catch (DuplicatePublicObjectId &e) {
        SCDETECT_LOG_WARNING_TAGGED(detectorId, "Internal error: %s", e.what());
        continue;
      }
    }
  }

  auto createTheoreticalTemplateArrivals{
      d->publishConfig.createTemplateArrivals};
  if (createTheoreticalTemplateArrivals) {
    for (const auto &a : d->publishConfig.theoreticalTemplateArrivals) {
      try {
        const auto pick{createPick(a, true)};
        const auto arrival{createArrival(a, *pick, ci)};
        detection.arrivalPicks.push_back({arrival, pick});
      } catch (DuplicatePublicObjectId &e) {
        SCDETECT_LOG_WARNING_TAGGED(detectionNotification.detectorId,
                                    "Internal error: %s", e.what());
        continue;
      }
    }
  }

  return true;
}

void Application::publishDetection(const Detection &detection) {
  logObject(_outputOrigins, Core::Time::GMT());

  if (connection() && !_config.noPublish) {
    SCDETECT_LOG_DEBUG_TAGGED(detection.detectorId,
                              "Sending event parameters (detection) ...");

    auto notifierMsg{util::make_smart<DataModel::NotifierMessage>()};

    // origin
    auto notifier{util::make_smart<DataModel::Notifier>(
        "EventParameters", DataModel::OP_ADD, detection.origin.get())};
    notifierMsg->attach(notifier.get());

    // comments
    for (std::size_t i{0}; i < detection.origin->commentCount(); ++i) {
      auto notifier{util::make_smart<DataModel::Notifier>(
          detection.origin->publicID(), DataModel::OP_ADD,
          detection.origin->comment(i))};

      notifierMsg->attach(notifier.get());
    }

    for (auto &arrivalPick : detection.arrivalPicks) {
      // pick
      {
        auto notifier{util::make_smart<DataModel::Notifier>(
            "EventParameters", DataModel::OP_ADD, arrivalPick.pick.get())};

        notifierMsg->attach(notifier.get());
      }
      // arrival
      {
        auto notifier{util::make_smart<DataModel::Notifier>(
            detection.origin->publicID(), DataModel::OP_ADD,
            arrivalPick.arrival.get())};

        notifierMsg->attach(notifier.get());
      }
    }

    if (!connection()->send(notifierMsg.get())) {
      SCDETECT_LOG_ERROR_TAGGED(
          detection.detectorId,
          "Sending of event parameters (detection) failed.");
    }
  }

  if (_ep) {
    _ep->add(detection.origin.get());

    for (auto &arrivalPick : detection.arrivalPicks) {
      detection.origin->add(arrivalPick.arrival.get());

      _ep->add(arrivalPick.pick.get());
    }
  }
}

Application::Config::Config() {
  Environment *env{Environment::Instance()};

  boost::filesystem::path scInstallDir{env->installDir()};
  boost::filesystem::path pathCache{scInstallDir /
                                    settings::kPathFilesystemCache};
  pathFilesystemCache = pathCache.string();
}

void Application::Config::init(const Client::Application *app) {
  try {
    pathTemplateJson = app->configGetPath("templatesJSON");
  } catch (...) {
  }
  try {
    const auto messagingGroup{
        app->configGetString("amplitudes.messagingGroup")};
    if (!messagingGroup.empty()) {
      amplitudeMessagingGroup = messagingGroup;
    }
  } catch (...) {
  }

  try {
    publishConfig.createArrivals = app->configGetBool("publish.createArrivals");
  } catch (...) {
  }
  try {
    publishConfig.createTemplateArrivals =
        app->configGetBool("publish.createTemplateArrivals");
  } catch (...) {
  }
  try {
    publishConfig.originMethodId = app->configGetString("publish.methodId");
  } catch (...) {
  }
  try {
    publishConfig.createAmplitudes =
        app->configGetBool("amplitudes.createAmplitudes");
  } catch (...) {
  }
  try {
    publishConfig.createMagnitudes =
        app->configGetBool("magnitudes.createMagnitudes");
  } catch (...) {
  }

  try {
    streamConfig.templateConfig.phase = app->configGetString("template.phase");
  } catch (...) {
  }
  try {
    streamConfig.templateConfig.wfStart =
        app->configGetDouble("template.waveformStart");
  } catch (...) {
  }
  try {
    streamConfig.templateConfig.wfEnd =
        app->configGetDouble("template.waveformEnd");
  } catch (...) {
  }

  try {
    forcedWaveformBufferSize =
        Core::TimeSpan{app->configGetDouble("processing.waveformBufferSize")};
  } catch (...) {
  }

  try {
    streamConfig.filter = app->configGetString("processing.filter");
  } catch (...) {
  }
  try {
    streamConfig.initTime = app->configGetDouble("processing.initTime");
  } catch (...) {
  }
  try {
    detectorConfig.gapInterpolation =
        app->configGetBool("processing.gapInterpolation");
  } catch (...) {
  }
  try {
    detectorConfig.gapThreshold =
        app->configGetDouble("processing.minGapLength");
  } catch (...) {
  }
  try {
    detectorConfig.gapTolerance =
        app->configGetDouble("processing.maxGapLength");
  } catch (...) {
  }

  try {
    detectorConfig.triggerOn =
        app->configGetDouble("detector.triggerOnThreshold");
  } catch (...) {
  }
  try {
    detectorConfig.triggerOff =
        app->configGetDouble("detector.triggerOffThreshold");
  } catch (...) {
  }
  try {
    detectorConfig.triggerDuration =
        app->configGetDouble("detector.triggerDuration");
  } catch (...) {
  }
  try {
    detectorConfig.timeCorrection =
        app->configGetDouble("detector.timeCorrection");
  } catch (...) {
  }
  try {
    detectorConfig.arrivalOffsetThreshold =
        app->configGetDouble("detector.arrivalOffsetThreshold");
  } catch (...) {
  }
  try {
    detectorConfig.minArrivals = app->configGetInt("detector.minimumArrivals");
  } catch (...) {
  }
  try {
    detectorConfig.mergingStrategy =
        app->configGetString("detector.mergingStrategy");
  } catch (...) {
  }

  try {
    sensorLocationBindings.amplitudeProcessingConfig.mlx.filter =
        app->configGetString("amplitudes.filter");
  } catch (ValueException &e) {
    throw;
  } catch (...) {
  }
  try {
    sensorLocationBindings.amplitudeProcessingConfig.mlx.initTime =
        app->configGetDouble("amplitudes.initTime");
  } catch (ValueException &e) {
    throw;
  } catch (...) {
  }
}

void Application::Config::init(const System::CommandLine &commandline) {
  templatesPrepare = commandline.hasOption("templates-prepare");
  templatesNoCache = commandline.hasOption("templates-reload");

  if (commandline.hasOption("templates-json")) {
    Environment *env{Environment::Instance()};
    pathTemplateJson =
        env->absolutePath(commandline.option<std::string>("templates-json"));
  }

  playbackConfig.enabled = commandline.hasOption("playback");

  offlineMode = commandline.hasOption("offline");
  noPublish = commandline.hasOption("no-publish");
}

}  // namespace detect
}  // namespace Seiscomp
