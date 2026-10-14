#ifndef PLUGINSTRUCTS_P251_DATA_STRUCT_H
#define PLUGINSTRUCTS_P251_DATA_STRUCT_H

#include "../../_Plugin_Helper.h"
#ifdef USES_P251

# include <deque>
# include <ESPeasySerial.h>

# include "P251_protocol.h"

# define P251_BAUD_MULT        PCONFIG(0)       // Baud rate / 9600: 1, 2, 4, 6 or 12
# define P251_LED_FEEDBACK     PCONFIG(1)       // 1 = show match / no match / enrolment on the LED ring
# define P251_MATCH_COLOR      PCONFIG(2)       // LED color of a match (P251_proto::COLOR_...)
# define P251_NOMATCH_COLOR    PCONFIG(3)       // LED color of no match
# define P251_POLL_MS          PCONFIG(4)       // Scan interval when no touch GPIO is set (ms)
# define P251_MIN_SCORE        PCONFIG(5)       // Matches with a lower score are reported as no match
# define P251_PASSWORD         PCONFIG_ULONG(0) // Module password (default 0)
# define P251_ADDRESS          PCONFIG_ULONG(1) // Module address (default 0xFFFFFFFF)

# define P251_DEF_BAUD_MULT    6                // 57600 baud
# define P251_DEF_POLL_MS      250
# define P251_DEF_CAPACITY     200              // R503; replaced by the library size read from the module

# define P251_VALUE_ID         0
# define P251_VALUE_SCORE      1
# define P251_VALUE_COUNT      2

// Error codes of the <taskname>#EnrollFailed and #Error events, besides the module confirmation codes
# define P251_ERR_NO_RESPONSE  0xFF             // The module didn't answer
# define P251_ERR_FINGER_TIMEOUT 0xFE           // No finger placed (or not lifted) within P251_FINGER_TIMEOUT_MS
# define P251_ERR_CANCELLED    0xFD             // fingerprint,cancel

enum class P251_Job : uint8_t {
  None,
  Init,   // Verify password, read the library size and the template count
  Scan,   // Capture, search, report
  Enroll, // Two captures, merge, store
  Delete,
  Empty,
  Count,
  Led,
};

enum class P251_Step : uint8_t {
  None,
  VerifyPassword,
  ReadParams,
  ReadCount,
  ScanImage,
  ScanFeatures,
  ScanSearch,
  ScanRemove,     // Wait until the finger is lifted (no touch GPIO)
  EnrollImage,
  EnrollFeatures,
  EnrollRemove,
  EnrollModel,
  EnrollStore,
  Delete,
  Empty,
  Led,
};

struct P251_request {
  P251_Job job;
  uint16_t id;
  uint8_t  led[4]; // mode, speed, color, times
};

struct P251_data_struct : public PluginTaskData_base {
public:

  P251_data_struct(struct EventStruct *event);

  P251_data_struct() = delete;
  virtual ~P251_data_struct();

  bool init(struct EventStruct *event);
  void loop(struct EventStruct *event);
  bool plugin_write(struct EventStruct *event,
                    const String      & string);
  void showValues(struct EventStruct *event) const;

private:

  bool request(const P251_request& req);
  void idle(struct EventStruct *event);
  void startJob(struct EventStruct *event,
                const P251_request& req);
  void startStep(P251_Step step,
                 uint32_t  delayMs = 0);
  void sendStep();
  void sendCommand(uint8_t        cmd,
                   const uint8_t *params,
                   size_t         nparams,
                   uint32_t       timeoutMs = 2000);
  void handleAck(struct EventStruct        *event,
                 const P251_proto::Packet& pkt);
  void handleTimeout(struct EventStruct *event);
  void handleEnrollAck(struct EventStruct *event,
                       uint8_t             code);
  void report(struct EventStruct *event,
              int                 id,
              int                 score);
  void enrollDone(struct EventStruct *event,
                  int                 errorCode);
  void led(uint8_t   mode,
           uint8_t   color,
           uint8_t   speed,
           uint8_t   times,
           P251_Step next);
  void finish();
  bool touched() const;
  void addEvent(struct EventStruct        *event,
                const __FlashStringHelper *name,
                int                        value) const;

  ESPeasySerial *_serial = nullptr;
  P251_proto::Parser _parser;
  std::deque<P251_request> _queue;

  // Settings
  uint32_t _address      = P251_proto::DEFAULT_ADDRESS;
  uint32_t _password     = 0;
  uint32_t _baud         = 57600;
  int8_t   _touchPin     = -1;
  bool     _ledFeedback  = true;
  uint8_t  _matchColor   = P251_proto::COLOR_BLUE;
  uint8_t  _noMatchColor = P251_proto::COLOR_RED;
  uint16_t _pollMs       = P251_DEF_POLL_MS;
  uint16_t _minScore     = 0;

  // Module state
  bool     _ready     = false;
  uint16_t _capacity  = P251_DEF_CAPACITY;
  int      _count     = -1;
  uint32_t _nextInit  = 0;
  uint32_t _nextScan  = 0;
  bool     _waitLift  = false; // Don't scan again until the finger is lifted

  // Current job
  P251_Job  _job      = P251_Job::None;
  P251_Step _step     = P251_Step::None;
  uint32_t  _stepAt   = 0;     // When to send the step
  bool      _waiting  = false; // Waiting for the acknowledge of the step
  uint32_t  _sentAt   = 0;
  uint32_t  _timeout  = 0;
  P251_request _req{};
  P251_Step _ledNext  = P251_Step::None;
  uint8_t   _capture  = 1;     // Enrolment: capture 1 or 2 (= char buffer)
  uint32_t  _fingerDeadline = 0;
  bool      _cancel   = false;
};

#endif // ifdef USES_P251
#endif // ifndef PLUGINSTRUCTS_P251_DATA_STRUCT_H
