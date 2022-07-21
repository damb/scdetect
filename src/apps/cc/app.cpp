#include "app.h"

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
#include <cstddef>
#include <exception>
#include <ios>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
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
#include "processing/processor.h"
#include "resamplerstore.h"
#include "util/memory.h"
#include "util/util.h"
#include "util/waveform_stream_id.h"
#include "version.h"
#include "worker/event/command.h"

namespace Seiscomp {
namespace detect {

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

bool dispatchNotification(int type, Core::BaseObject *obj) {
  using WorkerNotificationType = WorkerNotification::Type;
  if (type < static_cast<int>(WorkerNotificationType::kDetection)) {
    assert(obj);

    // XXX(damb): make sure the memory is freed
    WorkerNotificationCPtr workerNotificationOwner{
        static_cast<WorkerNotification *>(obj)};

    std::ostringstream oss;
    oss << workerNotificationOwner->threadId;

    switch (type) {
      case static_cast<int>(WorkerNotificationType::kInitializing):
        SCDETECT_LOG_DEBUG_TAGGED(oss.str(), "Worker initializing ...");
        break;
      case static_cast<int>(WorkerNotificationType::kInitialized):
        SCDETECT_LOG_DEBUG_TAGGED(oss.str(), "Worker initialized");
        break;
      case static_cast<int>(WorkerNotificationType::kRunning):
        SCDETECT_LOG_DEBUG_TAGGED(oss.str(), "Worker running ...");
        break;
      case static_cast<int>(WorkerNotificationType::kTerminating):
        SCDETECT_LOG_DEBUG_TAGGED(oss.str(), "Worker terminating ...");
        break;
      case static_cast<int>(WorkerNotificationType::kFinished):
        SCDETECT_LOG_DEBUG_TAGGED(oss.str(), "Worker finished");
        break;
      default:
        SCDETECT_LOG_WARNING("Unknown worker notification type");
        return false;
    }

    return true;
  }
  // dispatch detection
  // TODO TODO TODO

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

  auto worker{util::make_shared<DetectorWorker>(
      worker::RecordStream{recordStreamURL()}, std::move(detectors))};

  _detectorWorkers.emplace_back(worker);

  return !_detectorWorkers.empty();
}

std::vector<std::unique_ptr<Detector>> Application::createDetectors(
    std::ifstream &ifs, WaveformHandlerIface *waveformHandler) {
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
                              e.what()};
          } catch (builder::NoStream &e) {
            if (_config.skipTemplateIfNoStreamData) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              e.what()};
          } catch (builder::NoPick &e) {
            if (_config.skipTemplateIfNoPick) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              e.what()};
          } catch (builder::NoWaveformData &e) {
            if (_config.skipTemplateIfNoWaveformData) {
              SCDETECT_LOG_WARNING(
                  "Skipping template processor initialization: %s", e.what());
              continue;
            }
            throw ConfigError{"failed to create template processor: " +
                              e.what()};
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

        detector->setResultCallback(
            [this](const Detector *detector, const Record *record,
                   std::unique_ptr<const Detector::Detection> detection) {
              // TODO(damb): implement callback
              // processDetection(detector, record, std::move(detection));
            });

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
  for (auto &worker : _detectorWorkers) {
    _detectorWorkerThreads.emplace_back(std::thread{[worker]() { *worker(); }});
  }

  return true;
}

void Application::shutdownDetectorWorkers() {
  // shutdown detector workers
  for (auto &w : _detectorWorkers) {
    w->postEvent(
        worker::event::Command{worker::event::Command::Type::kShutdown});
  }

  for (auto &t : _detectorWorkerThreads) {
    if (t.joinable()) {
      try {
        t.join();
      } catch (const std::system_error &e) {
        SCDETECT_LOG_WARNING("Failed to stop worker: %s", e.what());
        continue;
      }
    }
  }
}

/* std::unique_ptr<DataModel::Comment> */
/* Application::createTemplateWaveformTimeInfoComment( */
/*     const detector::Detector::Detection::TemplateResult &templateResult) { */
/*   auto ret{util::make_unique<DataModel::Comment>()}; */
/*   ret->setId(settings::kTemplateWaveformTimeInfoPickCommentId); */
/*   ret->setText(templateResult.templateWaveformStartTime.iso() + */
/*                settings::kTemplateWaveformTimeInfoPickCommentIdSep + */
/*                templateResult.templateWaveformEndTime.iso() + */
/*                settings::kTemplateWaveformTimeInfoPickCommentIdSep + */
/*                templateResult.templateWaveformReferenceTime.iso()); */

/*   return ret; */
/* } */

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
