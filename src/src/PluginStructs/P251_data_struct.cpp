#include "../PluginStructs/P251_data_struct.h"

#ifdef USES_P251

# define P251_FINGER_TIMEOUT_MS 20000 // Enrolment: max wait for placing / lifting the finger
# define P251_FINGER_POLL_MS    100   // Enrolment: GenImg interval while waiting for the finger
# define P251_TOUCH_RETRY_MS    100   // Touch GPIO active but no image yet: retry interval
# define P251_REINIT_MS         5000  // Retry interval when the module doesn't answer
# define P251_MAX_QUEUE         8

using namespace P251_proto;

P251_data_struct::P251_data_struct(struct EventStruct *event) {
  const int mult = P251_BAUD_MULT;

  _address      = P251_ADDRESS;
  _password     = P251_PASSWORD;
  _baud         = 9600ul * ((mult >= 1 && mult <= 12) ? mult : P251_DEF_BAUD_MULT);
  _touchPin     = validGpio(CONFIG_PIN3) ? CONFIG_PIN3 : -1;
  _ledFeedback  = P251_LED_FEEDBACK != 0;
  _matchColor   = constrain(static_cast<int>(P251_MATCH_COLOR), 1, 7);
  _noMatchColor = constrain(static_cast<int>(P251_NOMATCH_COLOR), 1, 7);
  _pollMs       = constrain(static_cast<int>(P251_POLL_MS), 50, 5000);
  _minScore     = max(0, static_cast<int>(P251_MIN_SCORE));
}

P251_data_struct::~P251_data_struct() {
  delete _serial;
}

bool P251_data_struct::init(struct EventStruct *event) {
  const int8_t            rx   = CONFIG_PIN1;
  const int8_t            tx   = CONFIG_PIN2;
  const ESPEasySerialPort port = static_cast<ESPEasySerialPort>(CONFIG_PORT);

  _serial = new (std::nothrow) ESPeasySerial(port, rx, tx);

  if (nullptr == _serial) { return false; }
  _serial->begin(_baud);
  serialHelper_log_GpioDescription(port, rx, tx);

  if (_touchPin >= 0) {
    pinMode(_touchPin, INPUT_PULLUP);
  }

  UserVar.setFloat(event->TaskIndex, P251_VALUE_ID,    -1);
  UserVar.setFloat(event->TaskIndex, P251_VALUE_SCORE, 0);
  UserVar.setFloat(event->TaskIndex, P251_VALUE_COUNT, 0);
  _nextInit = millis();
  return true;
}

bool P251_data_struct::touched() const {
  // The WAKEUP output of the module is low while a finger touches the sensor
  return _touchPin >= 0 && digitalRead(_touchPin) == LOW;
}

void P251_data_struct::addEvent(struct EventStruct *event, const __FlashStringHelper *name, int value) const {
  if (Settings.UseRules) {
    eventQueue.add(event->TaskIndex, name, value);
  }
}

// ----------------------------------------------------------------------------------------------------
// Main loop, called 50 times per second
// ----------------------------------------------------------------------------------------------------

void P251_data_struct::loop(struct EventStruct *event) {
  if (nullptr == _serial) { return; }

  for (int available = _serial->available(); available > 0; --available) {
    const int c = _serial->read();

    if (c < 0) { break; }

    if (_parser.feed(static_cast<uint8_t>(c)) && _waiting && (_parser.packet().pid == PID_ACK)) {
      _waiting = false;
      handleAck(event, _parser.packet());
    }
  }

  if (_waiting) {
    if (timePassedSince(_sentAt) > static_cast<long>(_timeout)) {
      _waiting = false;
      handleTimeout(event);
    }
    return;
  }

  if (_cancel) {
    _cancel = false;

    if (_job == P251_Job::Enroll) {
      enrollDone(event, P251_ERR_CANCELLED);
      return;
    }
  }

  if (_step != P251_Step::None) {
    if (timeOutReached(_stepAt)) { sendStep(); }
    return;
  }

  idle(event);
}

void P251_data_struct::idle(struct EventStruct *event) {
  if (!_ready) {
    if (timeOutReached(_nextInit)) {
      _nextInit = millis() + P251_REINIT_MS;
      startJob(event, { P251_Job::Init, 0, {} });
    }
    return;
  }

  if (!_queue.empty()) {
    const P251_request req = _queue.front();
    _queue.pop_front();
    startJob(event, req);
    return;
  }

  if (_touchPin >= 0) {
    if (!touched()) {
      _waitLift = false;
      return;
    }

    if (_waitLift || !timeOutReached(_nextScan)) { return; }
    _nextScan = millis() + P251_TOUCH_RETRY_MS;
    startJob(event, { P251_Job::Scan, 0, {} });
    return;
  }

  // No touch GPIO: poll the module
  if (!timeOutReached(_nextScan)) { return; }
  _nextScan = millis() + _pollMs;

  if (_waitLift) {
    _job = P251_Job::Scan;
    startStep(P251_Step::ScanRemove);
  } else {
    startJob(event, { P251_Job::Scan, 0, {} });
  }
}

bool P251_data_struct::request(const P251_request& req) {
  if (_queue.size() >= P251_MAX_QUEUE) { return false; }
  _queue.push_back(req);
  return true;
}

void P251_data_struct::startJob(struct EventStruct *event, const P251_request& req) {
  _req    = req;
  _job    = req.job;
  _cancel = false;

  switch (_job) {
    case P251_Job::Init:   startStep(P251_Step::VerifyPassword); break;
    case P251_Job::Scan:   startStep(P251_Step::ScanImage);      break;
    case P251_Job::Delete: startStep(P251_Step::Delete);         break;
    case P251_Job::Empty:  startStep(P251_Step::Empty);          break;
    case P251_Job::Count:  startStep(P251_Step::ReadCount);      break;
    case P251_Job::Led:
      led(req.led[0], req.led[2], req.led[1], req.led[3], P251_Step::None);
      break;
    case P251_Job::Enroll:
      _capture        = 1;
      _fingerDeadline = millis() + P251_FINGER_TIMEOUT_MS;
      addLog(LOG_LEVEL_INFO, concat(F("Fingerprint: enrolling ID "), _req.id));
      addEvent(event, F("EnrollScan"), _capture);

      if (_ledFeedback) {
        led(AURA_BREATHING, COLOR_PURPLE, 100, 0, P251_Step::EnrollImage);
      } else {
        startStep(P251_Step::EnrollImage);
      }
      break;
    case P251_Job::None:
      finish();
      break;
  }
}

void P251_data_struct::startStep(P251_Step step, uint32_t delayMs) {
  _step   = step;
  _stepAt = millis() + delayMs;
}

void P251_data_struct::finish() {
  _job     = P251_Job::None;
  _step    = P251_Step::None;
  _ledNext = P251_Step::None;
}

void P251_data_struct::led(uint8_t mode, uint8_t color, uint8_t speed, uint8_t times, P251_Step next) {
  _req.led[0] = mode;
  _req.led[1] = speed;
  _req.led[2] = color;
  _req.led[3] = times;
  _ledNext    = next;
  startStep(P251_Step::Led);
}

// ----------------------------------------------------------------------------------------------------
// Commands to the module
// ----------------------------------------------------------------------------------------------------

void P251_data_struct::sendCommand(uint8_t cmd, const uint8_t *params, size_t nparams, uint32_t timeoutMs) {
  while (_serial->available() > 0) { _serial->read(); } // Drop late or unexpected bytes
  _parser.reset();

  uint8_t buf[MAX_CMD_SIZE];
  const size_t len = buildCommand(buf, _address, cmd, params, nparams);

  _serial->write(buf, len);
  _waiting = true;
  _sentAt  = millis();
  _timeout = timeoutMs;
}

void P251_data_struct::sendStep() {
  switch (_step) {
    case P251_Step::VerifyPassword:
    {
      const uint8_t p[] = {
        static_cast<uint8_t>(_password >> 24), static_cast<uint8_t>(_password >> 16),
        static_cast<uint8_t>(_password >> 8),  static_cast<uint8_t>(_password)
      };
      sendCommand(CMD_VFY_PWD, p, sizeof(p));
      break;
    }
    case P251_Step::ReadParams:
      sendCommand(CMD_READ_SYS_PARA, nullptr, 0);
      break;
    case P251_Step::ReadCount:
      sendCommand(CMD_TEMPLATE_NUM, nullptr, 0);
      break;
    case P251_Step::ScanImage:
    case P251_Step::ScanRemove:
    case P251_Step::EnrollImage:
    case P251_Step::EnrollRemove:
      sendCommand(CMD_GEN_IMG, nullptr, 0);
      break;
    case P251_Step::ScanFeatures:
    case P251_Step::EnrollFeatures:
    {
      const uint8_t p[] = { static_cast<uint8_t>(_step == P251_Step::ScanFeatures ? 1 : _capture) };
      sendCommand(CMD_IMG_2_TZ, p, sizeof(p));
      break;
    }
    case P251_Step::ScanSearch:
    {
      const uint8_t p[] = { 1, 0, 0, static_cast<uint8_t>(_capacity >> 8), static_cast<uint8_t>(_capacity) };
      sendCommand(CMD_SEARCH, p, sizeof(p), 3000);
      break;
    }
    case P251_Step::EnrollModel:
      sendCommand(CMD_REG_MODEL, nullptr, 0);
      break;
    case P251_Step::EnrollStore:
    {
      const uint8_t p[] = { 1, static_cast<uint8_t>(_req.id >> 8), static_cast<uint8_t>(_req.id) };
      sendCommand(CMD_STORE, p, sizeof(p));
      break;
    }
    case P251_Step::Delete:
    {
      const uint8_t p[] = { static_cast<uint8_t>(_req.id >> 8), static_cast<uint8_t>(_req.id), 0, 1 };
      sendCommand(CMD_DELETE, p, sizeof(p));
      break;
    }
    case P251_Step::Empty:
      sendCommand(CMD_EMPTY, nullptr, 0, 5000);
      break;
    case P251_Step::Led:
      sendCommand(CMD_AURA_LED, _req.led, sizeof(_req.led));
      break;
    case P251_Step::None:
      break;
  }
}

// ----------------------------------------------------------------------------------------------------
// Answers of the module
// ----------------------------------------------------------------------------------------------------

void P251_data_struct::handleAck(struct EventStruct *event, const Packet& pkt) {
  const uint8_t code = pkt.code();

  switch (_step) {
    case P251_Step::VerifyPassword:
      if (code == CODE_OK) {
        startStep(P251_Step::ReadParams);
      } else {
        addLog(LOG_LEVEL_ERROR, code == CODE_WRONG_PASSWORD
               ? String(F("Fingerprint: wrong module password"))
               : concat(F("Fingerprint: password check failed, code "), code));
        _nextInit = millis() + P251_REINIT_MS;
        finish();
      }
      break;

    case P251_Step::ReadParams:
      if (code == CODE_OK) {
        const uint16_t capacity = pkt.word(5);

        if ((capacity > 0) && (capacity <= 3000)) { _capacity = capacity; }
      }
      startStep(P251_Step::ReadCount);
      break;

    case P251_Step::ReadCount:
      if (code == CODE_OK) {
        _count = pkt.word(1);
        UserVar.setFloat(event->TaskIndex, P251_VALUE_COUNT, _count);
      }

      if (_job == P251_Job::Init) {
        _ready = true;
        addLogMove(LOG_LEVEL_INFO, strformat(F("Fingerprint: module ready, %d of %d templates used"), _count, _capacity));
      } else if (_job == P251_Job::Count) {
        addEvent(event, F("Count"), _count);
      }
      finish();
      break;

    case P251_Step::ScanImage:
      if (code == CODE_OK) {
        startStep(P251_Step::ScanFeatures);
      } else {
        finish(); // No finger (or bad image): try again at the next poll
      }
      break;

    case P251_Step::ScanFeatures:
      if (code == CODE_OK) {
        startStep(P251_Step::ScanSearch);
      } else {
        report(event, -1, 0); // Finger seen, but the image can't be used
      }
      break;

    case P251_Step::ScanSearch:
    {
      const int score = code == CODE_OK ? pkt.word(3) : 0;

      if ((code == CODE_OK) && (score >= _minScore)) {
        report(event, pkt.word(1), score);
      } else {
        report(event, -1, score);
      }
      break;
    }

    case P251_Step::ScanRemove:
      if (code == CODE_NO_FINGER) { _waitLift = false; }
      finish();
      break;

    case P251_Step::EnrollImage:
    case P251_Step::EnrollFeatures:
    case P251_Step::EnrollRemove:
    case P251_Step::EnrollModel:
    case P251_Step::EnrollStore:
      handleEnrollAck(event, code);
      break;

    case P251_Step::Delete:
    case P251_Step::Empty:
      if (code == CODE_OK) {
        addLog(LOG_LEVEL_INFO, _step == P251_Step::Empty
               ? String(F("Fingerprint: all templates deleted"))
               : concat(F("Fingerprint: deleted ID "), _req.id));
        addEvent(event, F("Deleted"), _step == P251_Step::Empty ? -1 : _req.id);
        startStep(P251_Step::ReadCount);
      } else {
        addLog(LOG_LEVEL_ERROR, concat(F("Fingerprint: delete failed, code "), code));
        addEvent(event, F("Error"), code);
        finish();
      }
      break;

    case P251_Step::Led:
      if (code != CODE_OK) {
        addLog(LOG_LEVEL_ERROR, concat(F("Fingerprint: LED command failed, code "), code));

        if (_job == P251_Job::Led) { addEvent(event, F("Error"), code); }
      }

      if (_ledNext != P251_Step::None) {
        startStep(_ledNext);
        _ledNext = P251_Step::None;
      } else {
        finish();
      }
      break;

    case P251_Step::None:
      break;
  }
}

void P251_data_struct::handleEnrollAck(struct EventStruct *event, uint8_t code) {
  switch (_step) {
    case P251_Step::EnrollImage:
      if (code == CODE_OK) {
        startStep(P251_Step::EnrollFeatures);
      } else if (timeOutReached(_fingerDeadline)) {
        enrollDone(event, P251_ERR_FINGER_TIMEOUT);
      } else {
        startStep(P251_Step::EnrollImage, P251_FINGER_POLL_MS);
      }
      break;

    case P251_Step::EnrollFeatures:
      if ((code == CODE_OK) && (_capture == 2)) {
        startStep(P251_Step::EnrollModel);
        break;
      }

      if (code != CODE_OK) {
        // Poor image: lift the finger and repeat the same capture
        addLog(LOG_LEVEL_INFO, concat(F("Fingerprint: image not usable, place the finger again, code "), code));
      }
      addEvent(event, F("EnrollRemove"), _capture);

      if (code == CODE_OK) { ++_capture; }
      _fingerDeadline = millis() + P251_FINGER_TIMEOUT_MS;
      startStep(P251_Step::EnrollRemove, P251_FINGER_POLL_MS);
      break;

    case P251_Step::EnrollRemove:
      if (code == CODE_NO_FINGER) {
        addEvent(event, F("EnrollScan"), _capture);
        _fingerDeadline = millis() + P251_FINGER_TIMEOUT_MS;
        startStep(P251_Step::EnrollImage, P251_FINGER_POLL_MS);
      } else if (timeOutReached(_fingerDeadline)) {
        enrollDone(event, P251_ERR_FINGER_TIMEOUT);
      } else {
        startStep(P251_Step::EnrollRemove, P251_FINGER_POLL_MS);
      }
      break;

    case P251_Step::EnrollModel:
      if (code == CODE_OK) {
        startStep(P251_Step::EnrollStore);
      } else {
        enrollDone(event, code); // 0x0A: the two captures are not the same finger
      }
      break;

    case P251_Step::EnrollStore:
      enrollDone(event, code);
      break;

    default:
      break;
  }
}

void P251_data_struct::handleTimeout(struct EventStruct *event) {
  if (_ready || (_job != P251_Job::Init)) {
    addLog(LOG_LEVEL_ERROR, F("Fingerprint: no answer from the module, check wiring and baud rate"));
  }
  const P251_Job job = _job;

  _ready    = false;
  _nextInit = millis() + P251_REINIT_MS;

  if (job == P251_Job::Enroll) {
    addEvent(event, F("EnrollFailed"), P251_ERR_NO_RESPONSE);
  } else if ((job == P251_Job::Delete) || (job == P251_Job::Empty) || (job == P251_Job::Led) || (job == P251_Job::Count)) {
    addEvent(event, F("Error"), P251_ERR_NO_RESPONSE);
  }
  finish();
}

// ----------------------------------------------------------------------------------------------------
// Results
// ----------------------------------------------------------------------------------------------------

void P251_data_struct::report(struct EventStruct *event, int id, int score) {
  UserVar.setFloat(event->TaskIndex, P251_VALUE_ID,    id);
  UserVar.setFloat(event->TaskIndex, P251_VALUE_SCORE, score);

  if (loglevelActiveFor(LOG_LEVEL_INFO)) {
    addLogMove(LOG_LEVEL_INFO, id >= 0
               ? strformat(F("Fingerprint: match ID %d, score %d"), id, score)
               : strformat(F("Fingerprint: no match (score %d)"), score));
  }
  sendData(event);
  _waitLift = true;

  if (_ledFeedback) {
    led(AURA_FLASHING, id >= 0 ? _matchColor : _noMatchColor, 30, 2, P251_Step::None);
  } else {
    finish();
  }
}

void P251_data_struct::enrollDone(struct EventStruct *event, int errorCode) {
  if (errorCode == CODE_OK) {
    addLog(LOG_LEVEL_INFO, concat(F("Fingerprint: enrolled ID "), _req.id));
    addEvent(event, F("Enrolled"), _req.id);
  } else {
    addLog(LOG_LEVEL_ERROR, concat(F("Fingerprint: enrolment failed, code "), errorCode));
    addEvent(event, F("EnrollFailed"), errorCode);
  }
  _waitLift = true; // The enrolled finger may still be on the sensor: don't report it as a scan

  const P251_Step next = errorCode == CODE_OK ? P251_Step::ReadCount : P251_Step::None;

  if (_ledFeedback) {
    led(AURA_FLASHING, errorCode == CODE_OK ? _matchColor : _noMatchColor, 30, 3, next);
  } else if (next != P251_Step::None) {
    startStep(next);
  } else {
    finish();
  }
}

// ----------------------------------------------------------------------------------------------------
// Commands: fingerprint,<subcommand>,...
// ----------------------------------------------------------------------------------------------------

bool P251_data_struct::plugin_write(struct EventStruct *event, const String& string) {
  if (!equals(parseString(string, 1), F("fingerprint"))) { return false; }

  const String  sub = parseString(string, 2);
  P251_request  req{};
  int32_t       value = 0;

  if (equals(sub, F("enroll")) || equals(sub, F("delete"))) {
    if (!validIntFromString(parseString(string, 3), value) || (value < 0) || (value >= _capacity)) {
      addLogMove(LOG_LEVEL_ERROR, strformat(F("Fingerprint: the ID must be 0..%d"), _capacity - 1));
      return true;
    }
    req.job = equals(sub, F("enroll")) ? P251_Job::Enroll : P251_Job::Delete;
    req.id  = value;
  } else if (equals(sub, F("deleteall"))) {
    req.job = P251_Job::Empty;
  } else if (equals(sub, F("count"))) {
    req.job = P251_Job::Count;
  } else if (equals(sub, F("led"))) {
    // fingerprint,led,<mode>,<color>[,<speed>[,<times>]]
    const uint8_t mode  = ledMode(parseString(string, 3).c_str());
    uint8_t       color = ledColor(parseString(string, 4).c_str());

    if ((mode == AURA_OFF) && (color == 0)) { color = COLOR_RED; }

    int32_t speed = 100, times = 0;
    const String speedArg = parseString(string, 5);
    const String timesArg = parseString(string, 6);

    if ((mode == 0) || (color == 0) ||
        (!speedArg.isEmpty() && !validIntFromString(speedArg, speed)) ||
        (!timesArg.isEmpty() && !validIntFromString(timesArg, times))) {
      addLog(LOG_LEVEL_ERROR, F("Fingerprint: use fingerprint,led,<mode>,<color>[,<speed>[,<times>]]"));
      return true;
    }
    req.job    = P251_Job::Led;
    req.led[0] = mode;
    req.led[1] = constrain(static_cast<int>(speed), 0, 255);
    req.led[2] = color;
    req.led[3] = constrain(static_cast<int>(times), 0, 255);
  } else if (equals(sub, F("cancel"))) {
    for (auto it = _queue.begin(); it != _queue.end();) {
      it = it->job == P251_Job::Enroll ? _queue.erase(it) : it + 1;
    }
    _cancel = _job == P251_Job::Enroll;
    return true;
  } else {
    addLog(LOG_LEVEL_ERROR, concat(F("Fingerprint: unknown command "), sub));
    return true;
  }

  if (!request(req)) {
    addLog(LOG_LEVEL_ERROR, F("Fingerprint: too many commands queued"));
  }
  return true;
}

void P251_data_struct::showValues(struct EventStruct *event) const {
  uint8_t varNr = VARS_PER_TASK;

  pluginWebformShowValue(event->TaskIndex, varNr++, F("Module"),
                         _ready ? String(F("OK")) : String(F("No answer")));
  pluginWebformShowValue(event->TaskIndex, varNr++, F("Templates"),
                         strformat(F("%d / %d"), _count, _capacity), true);
}

#endif // ifdef USES_P251
