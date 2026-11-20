#include <Arduino.h>
#include <EEPROM.h>
#include <usbd_cdc_if.h>
#include <miotyAtClient.h>
#include <stdarg.h>
#include "AtTestRunner.h"
#include "RfDmaRx.h"
#include "TestConfig.h"
#include "TestProtocol.h"
#include "FirmwareBuffers.h"

extern Uart SerialMioty;
#define TEST_STRING_INNER(x) #x
#define TEST_STRING(x) TEST_STRING_INNER(x)
#ifndef CLIENT_REV
#define CLIENT_REV unknown
#endif
namespace {
using TestProtocol::View;
using TestProtocol::Result;
enum class Op {
  Info, LibVersion, Reset, Factory, Defaults, Key, SubnetGet, SubnetSet, Eui,
  BaudGet, BaudSet, PowerGet, PowerSet, ModeGet, ModeSet, ProfileGet, ProfileSet,
  RateGet, RateSet, Abort, Available, Response, Format, Config, AttachInfo,
  Pending, Uni, Bidi, Message, OtaAttach, OtaDetach, LocalAttach, LocalDetach,
  PowerDown, Bootloader
};
enum class Expect { Success, ParserRangeError, AlreadyAttached, Detached, IdleAbort, NoReply, CapacityError };
struct Step {
  const char *name;
  Op op;
  int32_t value = 0;
  const uint8_t *data = nullptr;
  uint8_t size = 0;
  Expect expect = Expect::Success;
};
TestCapture &trace = firmwareBuffers.capture;
struct Outputs {
  char text[192];
  uint32_t integer, counter, ota, groups;
  uint8_t bytes[255], length, state, address[2], options[2];
} values;
bool stopped = false, logLost = false, capturing = false;
bool desynchronized = false;
unsigned recoveries = 0;
unsigned caseNumber = 0, counts[6] = {};
unsigned notRun = 0;
uint8_t payload[245], nonce[8];
uint32_t activeProfile = 0;
bool activeProfileKnown = false;
bool lastProfileSetAccepted = false, lastLocalDetachAccepted = false;
bool localDetachAttempted = false;
bool nextMessageClassA = false;
uint32_t availablePayload = 0;
bool availablePayloadKnown = false;
constexpr uint32_t sentinel = 0xDEADBEEF;

bool expired(uint32_t end) { return int32_t(millis() - end) >= 0; }
View rxView() { return {trace.rx, trace.rxSize}; }

void pump() {
  pollRfDmaRx();
  if (!capturing) return;
  const size_t start = trace.rxSize;
  for (unsigned budget = 0; budget < 256 && SerialMioty.available(); ++budget) {
    const int c = SerialMioty.read();
    if (c < 0) break;
    if (trace.rxSize < sizeof(trace.rx)) trace.rx[trace.rxSize++] = uint8_t(c);
    else ++trace.droppedRx;
  }
  if (trace.rxSize != start) {
    const uint32_t now = millis() - trace.started;
    if (trace.chunkCount && trace.chunks[trace.chunkCount - 1].offset +
        trace.chunks[trace.chunkCount - 1].size == start &&
        now - trace.chunks[trace.chunkCount - 1].ms < 2) {
      trace.chunks[trace.chunkCount - 1].size += trace.rxSize - start;
    } else if (trace.chunkCount < sizeof(trace.chunks) / sizeof(trace.chunks[0])) {
      trace.chunks[trace.chunkCount++] = {now, uint16_t(start), uint16_t(trace.rxSize - start)};
    } else trace.chunkOverflow = true;
    trace.lastRx = millis();
  }
  if (!CDC_connected()) stopped = true;
  for (unsigned i = 0; i < 32 && SerialUSB.available(); ++i) {
    if (SerialUSB.read() == 3) stopped = true;
  }
}

// USB writes never block the RF service. Losing the host stops the suite.
void emit(const char *s, size_t n) {
  uint32_t limit = millis() + 5000;
  while (n && !logLost) {
    pump();
    if (!CDC_connected() || expired(limit)) { logLost = stopped = true; break; }
    const int room = SerialUSB.availableForWrite();
    if (room > 0) {
      const size_t written = SerialUSB.write(reinterpret_cast<const uint8_t *>(s),
        n < size_t(room) ? n : size_t(room));
      s += written; n -= written;
      if (written) limit = millis() + 5000;
    }
  }
}
void log(const char *format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n > 0) emit(buffer, size_t(n) < sizeof(buffer) ? size_t(n) : sizeof(buffer) - 1);
}
void escaped(const uint8_t *data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const uint8_t c = data[i];
    if (c == '\r') emit("\\r", 2);
    else if (c == '\n') emit("\\n", 2);
    else if (c == '\t') emit("\\t", 2);
    else if (c == '\\') emit("\\\\", 2);
    else if (c >= 32 && c <= 126) emit(reinterpret_cast<const char *>(data + i), 1);
    else log("\\x%02X", c);
  }
}
void dump(const char *kind, const uint8_t *data, size_t size, size_t offset = 0) {
  // Bounded lines make log files easy to diff and copy without terminal wrapping.
  for (size_t at = 0; at < size; at += 32) {
    const size_t n = size - at < 32 ? size - at : 32;
    log("%s offset=%u length=%u HEX=", kind, unsigned(offset + at), unsigned(n));
    for (size_t i = 0; i < n; ++i) log("%02X", data[at + i]);
    emit(" TEXT=", 6); escaped(data + at, n); emit("\r\n", 2);
  }
}

const char *codeName(int code) {
  static const char *const names[] = {
    "OK", "MacError", "MacFramingError", "ArgumentSizeMismatch", "ArgumentOOR",
    "BufferSizeInsufficient", "MacNodeNotAttached", "MacNetworkKeyNotSet",
    "MacAlreadyAttached", "ERR", "MacDownlinkNotAvailable", "UplinkPackingErr",
    "MacNoDownlinkReceived", "MacOptionNotAllowed", "MacDownlinkErr", "MacDefaultsNotSet",
    "ATErr", "ATgenericErr", "ATCommandNotKnown", "ATParamOOB", "ATDataSizeMismatch",
    "ATUnexpectedChar", "ATArgInvalid", "ATReadFailed", "ATDataSizeOOB", "ATTimeout"
  };
  return code == 100 ? "Unsupported" : code >= 0 && code < 26 ? names[code] : "UNKNOWN";
}

bool radio(Op op) {
  return op == Op::Message || op == Op::Uni || op == Op::Bidi || op == Op::OtaAttach;
}
bool bidi(Op op) { return op == Op::Bidi || op == Op::OtaAttach; }
bool needsClassA(const Step &s) { return bidi(s.op) || (s.op == Op::Message && nextMessageClassA); }

void startCapture(uint32_t timeout) {
  // All preceding RF bytes must already have been logged by settle().
  firmwareBuffers.capture = {}; // Activate the union member before using trace.
  trace.started = trace.lastRx = millis();
  trace.deadline = trace.started + timeout;
  trace.before = rfRxStats();
  capturing = true;
}

void settle(const Step &s) {
  // Keep late events outside the client's parser, while preserving their bytes.
  const bool onAir = radio(s.op) && TestProtocol::result(rxView()).finalCode == 0;
  const uint32_t minWait = s.op == Op::Reset || s.op == Op::Factory ? 2000 : 250;
  const uint32_t end = onAir ? trace.started + TestConfig::radioTimeoutMs : millis() + minWait + 1500;
  const uint32_t started = millis();
  while (!stopped && !expired(end)) {
    pump();
    if (!onAir) {
      if (millis() - started >= minWait && millis() - trace.lastRx >= 200) break;
    } else {
      const bool complete = TestProtocol::radioComplete(rxView(), needsClassA(s));
      if (complete && millis() - trace.lastRx >= 250) break;
    }
  }
}

size_t expectedTx(const Step &s, uint8_t *out, size_t capacity) {
  const char *cmd = nullptr;
  bool bytesArg = false, integerArg = false, query = false;
  switch (s.op) {
    case Op::Info: cmd = "ATI"; break;
    case Op::LibVersion: cmd = "AT-LIBV"; break;
    case Op::Reset: cmd = "AT-RST"; break;
    case Op::Factory: cmd = "ATZ"; break;
    case Op::Defaults: cmd = "AT-DEF"; break;
    case Op::Key: cmd = "AT-MNWK"; bytesArg = true; break;
    case Op::SubnetGet: cmd = "AT-MSN6"; query = true; break;
    case Op::SubnetSet: cmd = "AT-MSN6"; bytesArg = true; break;
    case Op::Eui: cmd = "AT-MEUI"; query = true; break;
    case Op::BaudGet: cmd = "AT+IPR"; query = true; break;
    case Op::BaudSet: cmd = "AT+IPR"; integerArg = true; break;
    case Op::PowerGet: cmd = "AT-TXPL"; query = true; break;
    case Op::PowerSet: cmd = "AT-TXPL"; integerArg = true; break;
    case Op::ModeGet: cmd = "AT-UM"; query = true; break;
    case Op::ModeSet: cmd = "AT-UM"; integerArg = true; break;
    case Op::ProfileGet: cmd = "AT-UP"; query = true; break;
    case Op::ProfileSet: cmd = "AT-UP"; integerArg = true; break;
    case Op::RateGet: cmd = "AT-ZADR"; query = true; break;
    case Op::RateSet: cmd = "AT-ZADR"; integerArg = true; break;
    case Op::Abort: cmd = "AT-ABRT"; break;
    case Op::Available: cmd = "AT-MTRX"; query = true; break;
    case Op::Response: cmd = "AT-MRXO"; integerArg = true; break;
    case Op::Format: cmd = "AT-MEMPF"; integerArg = true; break;
    case Op::Config: cmd = "AT-MCFGF"; bytesArg = true; break;
    case Op::AttachInfo: cmd = "AT-MAI"; query = true; break;
    case Op::Pending: cmd = "AT-MPCS"; query = true; break;
    case Op::Uni: cmd = "AT-TU"; bytesArg = true; break;
    case Op::Bidi: cmd = "AT-TB"; bytesArg = true; break;
    case Op::Message: cmd = "AT-MTRX"; bytesArg = true; break;
    case Op::OtaAttach: cmd = "AT-MTAOA"; bytesArg = true; break;
    case Op::OtaDetach: cmd = "AT-MEDOA"; break;
    case Op::LocalAttach: cmd = "AT-MALO"; bytesArg = true; break;
    case Op::LocalDetach: cmd = "AT-MDLO"; break;
    case Op::PowerDown: cmd = "AT-PD"; break;
    case Op::Bootloader: cmd = "AT-SBTL"; break;
  }
  size_t n = strlen(cmd);
  if (n + 20 + 2 * s.size > capacity) return 0;
  memcpy(out, cmd, n);
  if (query) out[n++] = '?';
  if (integerArg) n += snprintf(reinterpret_cast<char *>(out + n), capacity - n, "=%ld", long(s.value));
  if (bytesArg) {
    const uint8_t config = uint8_t(s.value);
    const uint8_t *data = s.op == Op::Config ? &config : s.data;
    const size_t size = s.op == Op::Config ? 1 : s.size;
    n += snprintf(reinterpret_cast<char *>(out + n), capacity - n, "=%u\t", unsigned(size));
    const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
      out[n++] = hex[data[i] >> 4]; out[n++] = hex[data[i] & 15];
    }
    out[n++] = 0x1A;
  }
  if (s.op == Op::Available) out[n++] = 0x1A;
  out[n++] = '\r';
  return n;
}

miotyAtClient_returnCode invoke(const Step &s) {
  memset(&values, 0xA5, sizeof(values));
  values.text[0] = 0;
  values.text[sizeof(values.text) - 1] = 0;
  values.integer = s.value;
  values.counter = values.ota = values.groups = sentinel;
  values.state = 0xFF;
  values.length = sizeof(values.bytes);
  uint8_t data[255] = {};
  if (s.data && s.size) memcpy(data, s.data, s.size);
  switch (s.op) {
    case Op::Info: return miotyAtClient_getModemInfo(values.text, sizeof(values.text));
    case Op::LibVersion: return miotyAtClient_getCoreLibVersion(values.text, sizeof(values.text));
    case Op::Reset: return miotyAtClient_reset();
    case Op::Factory: return miotyAtClient_factoryReset();
    case Op::Defaults: return miotyAtClient_setDefaults();
    case Op::Key: return miotyAtClient_setNetworkKey(data);
    case Op::SubnetGet: return miotyAtClient_getOrSetIPv6SubnetMask(values.bytes, false);
    case Op::SubnetSet: return miotyAtClient_getOrSetIPv6SubnetMask(data, true);
    case Op::Eui: return miotyAtClient_getEui(values.bytes);
    case Op::BaudGet: return miotyAtClient_getOrSetBaudrate(&values.integer, false);
    case Op::BaudSet: return miotyAtClient_getOrSetBaudrate(&values.integer, true);
    case Op::PowerGet: return miotyAtClient_getOrSetTransmitPower(&values.integer, false);
    case Op::PowerSet: return miotyAtClient_getOrSetTransmitPower(&values.integer, true);
    case Op::ModeGet: return miotyAtClient_uplinkMode(&values.integer, false);
    case Op::ModeSet: return miotyAtClient_uplinkMode(&values.integer, true);
    case Op::ProfileGet: return miotyAtClient_uplinkProfile(&values.integer, false);
    case Op::ProfileSet: return miotyAtClient_uplinkProfile(&values.integer, true);
    case Op::RateGet: return miotyAtClient_dataRate(&values.integer, false);
    case Op::RateSet: return miotyAtClient_dataRate(&values.integer, true);
    case Op::Abort: return miotyAtClient_abort();
    case Op::Available: return miotyAtClient_getAvailablePayloadSize(&values.integer);
    case Op::Response: return miotyAtClient_setResponseFlag(s.value);
    case Op::Format: return miotyAtClient_setMacPayloadFormatField(s.value);
    case Op::Config: return miotyAtClient_setMacConfigFlag(s.value);
    case Op::AttachInfo: return miotyAtClient_getMacAttachInfo(&values.state, &values.ota,
      &values.groups, values.address, values.options);
    case Op::Pending: return miotyAtClient_getPendingUlCtrl(&values.integer);
    case Op::Uni: return miotyAtClient_sendMessageUniTransparent(data, s.size, &values.counter);
    case Op::Bidi: return miotyAtClient_sendMessageBidiTransparent(data, s.size, values.bytes,
      &values.length, &values.counter);
    case Op::Message: return miotyAtClient_sendMessage(data, s.size, &values.counter);
    case Op::OtaAttach: return miotyAtClient_macAttach(data, &values.state);
    case Op::OtaDetach: return miotyAtClient_macDetach();
    case Op::LocalAttach: return miotyAtClient_macAttachLocal(data, &values.state);
    case Op::LocalDetach: return miotyAtClient_macDetachLocal(&values.state);
    case Op::PowerDown: return miotyAtClient_powerDown();
    case Op::Bootloader: return miotyAtClient_startBootloader();
  }
  return MIOTYATCLIENT_RETURN_CODE_ERR;
}

bool compareBytes(const char *key, const uint8_t *output, size_t size) {
  View field;
  uint8_t decoded[255];
  size_t length = 0;
  if (!TestProtocol::field(rxView(), key, field) ||
      !TestProtocol::bytes(field, decoded, sizeof(decoded), length)) {
    log("CHECK FAIL missing/malformed byte field %s\r\n", key); return false;
  }
  if (length != size || memcmp(decoded, output, size) != 0) {
    log("CHECK FAIL API bytes differ from raw %s length=%u expected_length=%u\r\n",
        key, unsigned(size), unsigned(length)); return false;
  }
  return true;
}
bool compareInt(const char *key, uint32_t output, bool signedValue = false) {
  int64_t raw;
  if (!TestProtocol::integer(rxView(), key, raw)) {
    log("CHECK FAIL missing/malformed integer field %s\r\n", key); return false;
  }
  const int64_t actual = signedValue ? int64_t(int32_t(output)) : int64_t(output);
  log("CHECK field=%s raw=%ld api=%ld\r\n", key, long(raw), long(actual));
  if (raw != actual) { log("CHECK FAIL integer mismatch\r\n"); return false; }
  return true;
}

bool checkOutputs(const Step &s) {
  switch (s.op) {
    case Op::Info:
    case Op::LibVersion: {
      View field;
      const char *key = s.op == Op::Info ? "I" : "LIBV";
      log("API text="); escaped(reinterpret_cast<uint8_t *>(values.text), strlen(values.text));
      log("\r\n");
      if (!TestProtocol::field(rxView(), key, field) || !field.equals(values.text)) {
        log("CHECK FAIL string differs from raw response (including case)\r\n"); return false;
      }
      return true;
    }
    case Op::Eui: dump("API EUI", values.bytes, 8); return compareBytes("MEUI", values.bytes, 8);
    case Op::SubnetGet: {
      dump("API SUBNET", values.bytes, 8);
      View ignored;
      const bool matches = compareBytes(TestProtocol::field(rxView(), "MSN6", ignored) ? "MSN6" : "MIP6", values.bytes, 8);
      return matches && memcmp(values.bytes, TestConfig::subnet, 8) == 0;
    }
    case Op::BaudGet: return compareInt("+IPR", values.integer);
    case Op::PowerGet: return compareInt("TXPL", values.integer, true);
    case Op::ModeGet: return compareInt("UM", values.integer) && (s.value < 0 || values.integer == uint32_t(s.value));
    case Op::ProfileGet: return compareInt("UP", values.integer) && (s.value < 0 || values.integer == uint32_t(s.value));
    case Op::RateGet: return compareInt("ZADR", values.integer) && (s.value < 0 || values.integer == uint32_t(s.value));
    case Op::Available: return compareInt("MTRX", values.integer);
    case Op::Pending: {
      View field;
      uint8_t bytes[4]; size_t n = 0; int64_t raw;
      if (!TestProtocol::field(rxView(), "MAC_CHG_PEND_UL_CTRL", field)) return false;
      if (TestProtocol::bytes(field, bytes, sizeof(bytes), n) && n == 4) {
        raw = uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 | uint32_t(bytes[2]) << 8 | bytes[3];
      } else if (!TestProtocol::number(field, raw, 16)) return false;
      log("API ctrl=0x%08lX raw=0x%08lX\r\n", (unsigned long)values.integer, (unsigned long)raw);
      return uint32_t(raw) == values.integer && (values.integer & uint32_t(s.value)) == uint32_t(s.value);
    }
    case Op::AttachInfo: {
      log("API state=%u ota=%lu groups=%lu\r\n", values.state,
          (unsigned long)values.ota, (unsigned long)values.groups);
      bool good = compareInt("MAC_ATTACHSTATE", values.state);
      if (s.value >= 0 && values.state != s.value) {
        log("CHECK FAIL attachment state expected=%ld\r\n", long(s.value)); good = false;
      }
      good = compareInt("MAC_OTAATTACHCNT", values.ota) && good;
      good = compareInt("MAC_JOINEDGRPCNT", values.groups) && good;
      if (values.state == 2 || values.state == 5) {
        dump("API SHORTADDRESS", values.address, 2); dump("API LINKOPTIONS", values.options, 2);
        good = compareBytes("MAC_SHORTADDRESS", values.address, 2) && good;
        good = compareBytes("MAC_LINKOPTIONS", values.options, 2) && good;
      }
      return good;
    }
    case Op::LocalAttach:
    case Op::LocalDetach:
    case Op::OtaAttach: {
      log("API MSTA=%u (255=unknown for local detach; otherwise untouched)\r\n", values.state);
      // Only fields available by API return can be expected in this output.
      int64_t state;
      if (TestProtocol::integer({trace.rx, trace.apiRxSize}, "MAC_ATTACHSTATE", state))
        return state == values.state;
      View optionalState;
      if (s.op == Op::LocalDetach && !TestProtocol::field(rxView(), "MAC_ATTACHSTATE", optionalState)) {
        log("CHECK MDLO omitted optional state event; MSTA must be 255 (unknown)\r\n");
        return values.state == 255;
      }
      if (s.op != Op::OtaAttach) {
        log("CHECK FAIL local state arrived after API return or is missing\r\n"); return false;
      }
      return true;
    }
    case Op::Bidi: {
      log("API downlink_length=%u counter=%lu (3735928559=untouched)\r\n",
        values.length, (unsigned long)values.counter);
      dump("API DOWNLINK", values.bytes, values.length);
      View field;
      if (TestProtocol::field(rxView(), "A_DATA", field)) return compareBytes("A_DATA", values.bytes, values.length);
      return values.length == 0 && TestProtocol::event(rxView(), "A_ACTIVE", 0);
    }
    case Op::Uni:
    case Op::Message: {
      log("API counter=%lu (3735928559=untouched; MPCT is not required by v3)\r\n",
        (unsigned long)values.counter);
      View field;
      return !TestProtocol::field(rxView(), "MPCT", field) || compareInt("MPCT", values.counter);
    }
    default: return true;
  }
}

// Verdict codes are intentionally distinct: a device rejection is not a passed positive test.
enum Verdict { Pass, Fail, DeviceError, Unsupported, Inconclusive, SentUnverified };
const char *const verdictNames[] = {"PASS", "FAIL", "DEVICE_ERROR", "UNSUPPORTED", "INCONCLUSIVE", "SENT_UNVERIFIED"};

bool run(const Step &s);
bool recover() {
  ++recoveries;
  desynchronized = false;
  log("RECOVERY reset + ATI after unfinished command; following prerequisites may need inspection\r\n");
  run({"RECOVERY / soft reset", Op::Reset});
  run({"RECOVERY / readiness probe", Op::Info});
  if (TestProtocol::result(rxView()).finalCode != 0 || trace.droppedRx || trace.writeFailed) {
    stopped = true;
    log("RECOVERY failed; remaining cases are NOT_RUN\r\n");
    return false;
  }
  desynchronized = false;
  return !stopped;
}

bool run(const Step &s) {
  if (!stopped && desynchronized) recover();
  if (stopped) {
    ++notRun;
    log("NOT_RUN name=%s reason=suite_stopped\r\n", s.name);
    return false;
  }
  log("\r\n=== CASE %u %s ===\r\n", ++caseNumber, s.name);
  log("EXPECT kind=%u value=%ld payload_length=%u\r\n", unsigned(s.expect), long(s.value), s.size);
  startCapture(radio(s.op) ? TestConfig::radioTimeoutMs : TestConfig::commandTimeoutMs);
  const miotyAtClient_returnCode rc = invoke(s);
  trace.apiEnded = millis(); trace.apiRxSize = trace.rxSize;
  settle(s);
  uint8_t wire[600];
  const size_t expected = expectedTx(s, wire, sizeof(wire));
  bool good = expected && expected == trace.txSize && memcmp(wire, trace.tx, expected) == 0;
  const uint32_t duration = millis() - trace.started;
  log("TIME total_ms=%lu api_ms=%lu tx_ms=%lu\r\n", (unsigned long)duration,
    (unsigned long)(trace.apiEnded - trace.started), (unsigned long)trace.txMs);
  dump("TX", trace.tx, trace.txSize);
  if (!good) { log("CHECK FAIL transmitted bytes differ from specification\r\n"); dump("EXPECTED_TX", wire, expected); }
  size_t dumped = trace.rxSize;
  dump("RX", trace.rx, dumped);
  log("API rc=%d name=%s read_bytes=%u captured_at_return=%u read_timeouts=%lu\r\n", int(rc),
      codeName(rc), unsigned(trace.readCursor), unsigned(trace.apiRxSize), (unsigned long)trace.timeoutCount);
  log("API values integer=0x%08lX signed=%ld state=%u counter=%lu ota=%lu groups=%lu length=%u\r\n",
      (unsigned long)values.integer, long(int32_t(values.integer)), values.state,
      (unsigned long)values.counter, (unsigned long)values.ota, (unsigned long)values.groups, values.length);
  const Result raw = TestProtocol::result(rxView());
  log("RAW final=%d info_present=%u info=%d parser_present=%u parser=%d mac_error=%u\r\n",
    raw.finalCode, raw.hasInfo, raw.info, raw.hasParserError, raw.parserError, raw.hasMacError);
  Verdict verdict = Pass;
  if (s.expect == Expect::NoReply) {
    // PD/SBTL cannot be proved by a successful write or by silence.
    verdict = SentUnverified;
    if (raw.finalCode == 100 || (raw.hasInfo && raw.info == -100)) verdict = Unsupported;
    good = good && rc == MIOTYATCLIENT_RETURN_CODE_OK && raw.finalCode != 1 && raw.finalCode != 2;
    log("CHECK physical power-down/bootloader state requires manual observation\r\n");
  } else if (s.op == Op::Reset && raw.finalCode < 0) {
    good = good && rc == MIOTYATCLIENT_RETURN_CODE_OK;
    log("CHECK AT-RST has no required final result; following ATI tests readiness\r\n");
  } else if (raw.finalCode < 0) {
    verdict = Inconclusive;
    good = good && rc != MIOTYATCLIENT_RETURN_CODE_OK;
    log("CHECK no complete final result; inspect timeout, baud, firmware support and RX errors\r\n");
  } else {
    const int mapped = TestProtocol::mappedCode(raw);
    if (mapped != int(rc)) {
      good = false;
      log("CHECK FAIL raw result maps to rc=%d (%s), API returned rc=%d\r\n", mapped, codeName(mapped), int(rc));
    }
    if (raw.finalCode == 100 || (raw.hasInfo && raw.info == -100)) verdict = Unsupported;
    else if (s.expect == Expect::ParserRangeError) {
      good = good && raw.finalCode == 2 && raw.hasParserError && raw.parserError == 3;
    } else if (s.expect == Expect::AlreadyAttached || s.expect == Expect::Detached) {
      const int wanted = s.expect == Expect::AlreadyAttached ? -4 : -3;
      good = good && raw.finalCode == 1 && raw.hasInfo && raw.info == wanted;
    } else if (s.expect == Expect::CapacityError) {
      good = good && raw.finalCode == 1 && raw.hasInfo && raw.info == -1;
    } else if (s.expect == Expect::IdleAbort) {
      good = good && (raw.finalCode == 0 || (raw.finalCode == 1 && raw.hasInfo && raw.info == -8));
    } else if (raw.finalCode != 0) verdict = DeviceError;
    if (raw.finalCode == 0 && s.expect == Expect::Success) {
      View baudField;
      if (s.op == Op::BaudGet && TestProtocol::field(rxView(), "+IPR", baudField) &&
          memchr(baudField.data, ',', baudField.size)) {
        log("CHECK manual 4.1.1 ambiguously shows a baud list for IPR?; scalar API output is not verifiable\r\n");
        verdict = Inconclusive;
      } else {
        Step checked = s;
        if (s.op == Op::ProfileGet && s.value >= 0 && !lastProfileSetAccepted) {
          checked.value = -1;
          verdict = Inconclusive;
          log("CHECK preceding profile setter was rejected; only raw/API parsing can be verified\r\n");
        }
        if (s.op == Op::AttachInfo && s.value == 0 && localDetachAttempted && !lastLocalDetachAccepted &&
            TestProtocol::event(rxView(), "MAC_ATTACHSTATE", 2)) {
          checked.value = -1;
          verdict = Inconclusive;
          log("CHECK preceding local detach did not succeed; detached prerequisite is not met\r\n");
        }
        good = checkOutputs(checked) && good;
      }
    }
  }
  if (radio(s.op) && raw.finalCode == 0) {
    const bool complete = TestProtocol::radioComplete(rxView(), needsClassA(s));
    int64_t z = -1, a = -1;
    TestProtocol::lastInteger(rxView(), "Z_ACTIVE", z);
    TestProtocol::lastInteger(rxView(), "A_ACTIVE", a);
    log("RADIO local_job_closed=%u z_last=%ld a_last=%ld require_class_a=%u gateway_available=%u end_to_end=NOT_VERIFIED\r\n",
        complete, long(z), long(a), needsClassA(s), TestConfig::gatewayAvailable);
    if (!complete && verdict == Pass) verdict = Inconclusive;
    if (s.op == Op::OtaAttach) {
      // The final state can be an asynchronous event; report every one in raw trace.
      log("RADIO ota_attached_event=%u (no gateway: success is not expected)\r\n",
          TestProtocol::event(rxView(), "MAC_ATTACHSTATE", 2));
    }
  }
  // Printing/checking also services RX; never silently lose events arriving during log output.
  for (unsigned round = 0; round < 4 && trace.rxSize > dumped; ++round) {
    const size_t end = trace.rxSize;
    dump("LATE_RX", trace.rx + dumped, end - dumped, dumped); dumped = end;
  }
  const RfRxStats after = rfRxStats();
  log("TRANSPORT tx_overflow=%u rx_dropped=%lu timing_overflow=%u write_failed=%u uart_errors=%lu dma_lost_bytes=%lu dma_errors=%lu\r\n",
    trace.txOverflow, (unsigned long)trace.droppedRx, trace.chunkOverflow, trace.writeFailed,
    (unsigned long)(after.uartErrors - trace.before.uartErrors),
    (unsigned long)(after.overwrittenBytes - trace.before.overwrittenBytes),
    (unsigned long)(after.dmaErrors - trace.before.dmaErrors));
  for (size_t i = 0; i < trace.chunkCount; ++i)
    log("RX_TIME ms=%lu offset=%u length=%u\r\n", (unsigned long)trace.chunks[i].ms,
        trace.chunks[i].offset, trace.chunks[i].size);
  if (trace.txOverflow || trace.droppedRx || trace.chunkOverflow || trace.writeFailed ||
      after.uartErrors != trace.before.uartErrors || after.overwrittenBytes != trace.before.overwrittenBytes ||
      after.dmaErrors != trace.before.dmaErrors || trace.rxSize != dumped) good = false;
  if (trace.rxSize != dumped) log("CHECK FAIL unlogged trailing bytes=%u\r\n", unsigned(trace.rxSize - dumped));
  if (!good) verdict = Fail;
  if (stopped && verdict != Fail) verdict = Inconclusive;
  ++counts[verdict];
  log("RESULT case=%u verdict=%s\r\n", caseNumber, verdictNames[verdict]);
  // Stop after uncertain framing: never let a late response satisfy the next case.
  if (raw.finalCode < 0 && s.op != Op::Reset && s.expect != Expect::NoReply) desynchronized = true;
  if (radio(s.op) && raw.finalCode == 0 && !TestProtocol::radioComplete(rxView(), needsClassA(s)))
    desynchronized = true;
  if (raw.finalCode == 1 && raw.hasInfo && raw.info == -7) desynchronized = true;
  if (s.op == Op::ProfileGet && raw.finalCode == 0 && rc == MIOTYATCLIENT_RETURN_CODE_OK) {
    activeProfile = values.integer;
    activeProfileKnown = activeProfile <= 7;
  }
  if (s.op == Op::ProfileSet) lastProfileSetAccepted = raw.finalCode == 0;
  if (s.op == Op::LocalDetach) {
    localDetachAttempted = true;
    lastLocalDetachAccepted = raw.finalCode == 0;
  }
  if (s.op == Op::Response && raw.finalCode == 0) nextMessageClassA = true;
  if (s.op == Op::Message && raw.finalCode == 0) nextMessageClassA = false;
  if (s.op == Op::Pending && raw.finalCode == 0 && rc == MIOTYATCLIENT_RETURN_CODE_OK)
    nextMessageClassA = (values.integer & 4) != 0;
  if (s.op == Op::Available) {
    int64_t capacity;
    availablePayloadKnown = raw.finalCode == 0 &&
      TestProtocol::integer(rxView(), "MTRX", capacity) && capacity >= 0 && capacity <= 245;
    if (availablePayloadKnown) availablePayload = uint32_t(capacity);
  }
  if (s.op == Op::Reset || s.op == Op::Factory) {
    nextMessageClassA = false;
    availablePayloadKnown = false;
  }
  capturing = false;
  return !stopped;
}

bool reserveNonce() {
#if defined(DATA_EEPROM_BASE)
  // Reserve one previously untouched EEPROM word before transmitting. Both erased
  // 00000000 (STM32 data EEPROM) and FFFFFFFF are accepted as virgin words.
  // A torn/nonzero reservation is consumed forever, including after power loss.
  // The first 1024 bytes are dedicated to this test firmware. Never clear them.
  for (unsigned slot = 0; slot < 256; ++slot) {
    uint32_t word;
    EEPROM.get(slot * 4, word);
    if (word != 0 && word != UINT32_MAX) continue;
    const uint32_t mark = 0x51A7C03D;
    EEPROM.put(slot * 4, mark);
    EEPROM.get(slot * 4, word);
    if (word != mark) return false;
    const uint32_t device = HAL_GetUIDw0() ^ HAL_GetUIDw1() ^ HAL_GetUIDw2();
    for (unsigned i = 0; i < 4; ++i) {
      nonce[i] = device >> (24 - 8 * i);
      nonce[4 + i] = slot >> (24 - 8 * i);
    }
    return true;
  }
#endif
  return false;
}

void summary() {
  log("\r\n=== SUITE_END cases=%u not_run=%u stopped=%u log_lost=%u recoveries=%u ===\r\n", caseNumber, notRun, stopped, logLost, recoveries);
  for (unsigned i = 0; i < 6; ++i) log("SUMMARY %s=%u\r\n", verdictNames[i], counts[i]);
  log("COVERAGE planned APIs=29; automatic=27; manual=AT-PD,AT-SBTL; stopped=1 means remaining cases NOT_RUN\r\n");
  log("COVERAGE v2 removed: MSAD,MPCT,US,ACM,ACK,U,B,UMPF,BMPF,UTPL (not sent)\r\n");
  log("COVERAGE Class B/C and certification commands have no client API (not sent)\r\n");
  log("NETWORK uplink reception, OTA attach, ACK and downlink delivery: NOT_VERIFIED (no gateway)\r\n");
  log("Enter RUN to repeat, PD or SBTL for separate terminal tests. Ctrl-C stops a running suite.\r\n");
}
} // namespace

extern "C" void miotyAtClientWrite(uint8_t *buffer, uint16_t length) {
  if (!capturing) return;
  trace.txMs = millis() - trace.started;
  size_t offset = 0;
  while (offset < length && !stopped && !expired(trace.deadline)) {
    pump();
    const int room = SerialMioty.availableForWrite();
    if (room > 0) {
      size_t n = length - offset;
      if (n > size_t(room)) n = room;
      if (n > 32) n = 32;
      offset += SerialMioty.write(buffer + offset, n);
    }
  }
  // Record only bytes actually accepted by the hardware serial driver.
  if (offset <= sizeof(trace.tx) - trace.txSize) {
    memcpy(trace.tx + trace.txSize, buffer, offset); trace.txSize += offset;
  } else trace.txOverflow = true;
  // Write's void contract cannot propagate failures. Report them in the verdict.
  if (offset != length) trace.writeFailed = true;
  // Drain the UART shift register, not just its software TX ring, before reset/PD.
  while (!(SerialMioty.getHandle()->Instance->ISR & USART_ISR_TC) && !stopped && !expired(trace.deadline)) pump();
  if (!(SerialMioty.getHandle()->Instance->ISR & USART_ISR_TC)) trace.writeFailed = true;
}

extern "C" bool miotyAtClientRead(uint8_t *buffer, uint8_t *length) {
  if (!capturing || !length || !*length) return false;
  const uint8_t capacity = *length;
  *length = 0;
  while (!stopped && !expired(trace.deadline)) {
    pump();
    const size_t remaining = trace.rxSize - trace.readCursor;
    if (remaining) {
      *length = remaining < capacity ? remaining : capacity;
      memcpy(buffer, trace.rx + trace.readCursor, *length);
      trace.readCursor += *length;
      return true;
    }
  }
  ++trace.timeoutCount;
  return false;
}

void runAtTests() {
  stopped = logLost = capturing = false;
  desynchronized = false; recoveries = 0;
  caseNumber = notRun = 0;
  activeProfileKnown = lastProfileSetAccepted = lastLocalDetachAccepted = false;
  localDetachAttempted = false;
  nextMessageClassA = false;
  availablePayloadKnown = false;
  memset(counts, 0, sizeof(counts));
  for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = uint8_t(i);
  log("\r\n=== MIOTY_V3_SUITE_BEGIN protocol=3.0.0-12 build=%s_%s ===\r\n", __DATE__, __TIME__);
  log("CONFIG RF=%lu 8N1 PC10=TX PC11=RX HSI+DMA USB=115200\r\n", (unsigned long)TestConfig::rfBaud);
  log("CONFIG dedicated laboratory device; public test key; factory resets enabled; gateway=NO\r\n");
  log("NOTE ABI client enum=0..25,100; no client source modifications; raw data preserves case/CR/LF/SUB\r\n");
  log("CLIENT git=%s (see build/manifest.json for source hash)\r\n", TEST_STRING(CLIENT_REV));
  // Existing manual-mode output is drained before runAtTests is entered.
  run({"RST / no final reply required", Op::Reset});
  run({"ATI / modem identity and exact string", Op::Info});
  run({"LIBV / exact library string", Op::LibVersion});
  run({"MEUI / read only", Op::Eui});
  run({"ATZ / factory reset", Op::Factory});
  run({"ATI / ready after ATZ", Op::Info});
  run({"DEF / optional test defaults", Op::Defaults});
  run({"ATI / ready after DEF", Op::Info});
  run({"IPR / query (manual has scalar/list ambiguity)", Op::BaudGet});
  run({"IPR / set existing link speed", Op::BaudSet, int32_t(TestConfig::rfBaud)});
  run({"TXPL / query", Op::PowerGet});
  run({"TXPL / signed -3", Op::PowerSet, -3});
  run({"TXPL / read quantized signed value", Op::PowerGet});
  run({"TXPL / set 0 dBm", Op::PowerSet, 0});
  run({"TXPL / reject +31", Op::PowerSet, 31, nullptr, 0, Expect::ParserRangeError});
  run({"TXPL / reject -31", Op::PowerSet, -31, nullptr, 0, Expect::ParserRangeError});
  run({"UM / query", Op::ModeGet, -1});
  run({"UM / low latency", Op::ModeSet, 1});
  run({"UM / verify low latency", Op::ModeGet, 1});
  run({"UM / standard", Op::ModeSet, 0});
  run({"UM / reject 2", Op::ModeSet, 2, nullptr, 0, Expect::ParserRangeError});
  run({"UP / query", Op::ProfileGet, -1});
  if (TestConfig::profile >= 0 || activeProfileKnown) {
    const int32_t target = TestConfig::profile >= 0 ? TestConfig::profile : int32_t(activeProfile);
    run({"UP / laboratory profile", Op::ProfileSet, target});
    run({"UP / readback", Op::ProfileGet, target});
  } else {
    notRun += 2;
    log("NOT_RUN name=UP_set_and_readback reason=no_valid_profile_query\r\n");
  }
  run({"UP / reject 8", Op::ProfileSet, 8, nullptr, 0, Expect::ParserRangeError});
  run({"ZADR / query", Op::RateGet, -1});
  run({"ZADR / HDR", Op::RateSet, 1});
  run({"ZADR / HDR readback", Op::RateGet, 1});
  run({"ZADR / ULP", Op::RateSet, 0});
  run({"ZADR / reject 2", Op::RateSet, 2, nullptr, 0, Expect::ParserRangeError});
  run({"ABRT / idle (0 or INFO:-8)", Op::Abort, 0, nullptr, 0, Expect::IdleAbort});
  run({"MAI / initial attachment", Op::AttachInfo, 0});
  run({"MNWK / 16 byte laboratory key", Op::Key, 0, TestConfig::networkKey, 16});
  run({"MSN6 / set eight bytes", Op::SubnetSet, 0, TestConfig::subnet, 8});
  run({"MSN6 / read back", Op::SubnetGet});
  run({"MCFGF / disable IPv6 while detached", Op::Config, 0});
  run({"MPCS / initial pending flags", Op::Pending});
  run({"MTRX / available payload", Op::Available});
  run({"MTRX / detached error", Op::Message, 0, payload, 6, Expect::Detached});
  run({"MALO / local attachment", Op::LocalAttach, 0, TestConfig::shortAddress, 2});
  run({"MAI / attached fields", Op::AttachInfo, 2});
  run({"MALO / reject duplicate attachment", Op::LocalAttach, 0, TestConfig::shortAddress, 2, Expect::AlreadyAttached});
  run({"MCFGF / reject while attached", Op::Config, 0, nullptr, 0, Expect::AlreadyAttached});
  run({"MRXO / response flag 1", Op::Response, 1});
  run({"MPCS / flags after MRXO", Op::Pending, 6});
  run({"MRXO / response flag 0", Op::Response, 0});
  run({"MEMPF / field 255", Op::Format, 255});
  run({"MPCS / flags after MEMPF", Op::Pending, 1});
  run({"MEMPF / reject 256", Op::Format, 256, nullptr, 0, Expect::ParserRangeError});
  run({"MTRX / short payload", Op::Message, 0, payload, 6});
  run({"MTRX / empty control message", Op::Message, 0, payload, 0});
  run({"MTRX / 100 bytes (three digit length)", Op::Message, 0, payload, 100});
  run({"MTRX / payload capacity after control data", Op::Available});
  // 245 bytes is the protocol maximum, but pending MAC data can reduce capacity.
  const bool exceedsCapacity = availablePayloadKnown && availablePayload < 245;
  log("BOUNDARY requested=245 capacity_known=%u available=%lu expect_capacity_error=%u\r\n",
      availablePayloadKnown, (unsigned long)availablePayload, exceedsCapacity);
  run({"MTRX / 245 byte protocol boundary", Op::Message, 0, payload, 245,
       exceedsCapacity ? Expect::CapacityError : Expect::Success});
  run({"TU / transparent payload incl sync byte", Op::Uni, 0, payload, 6});
  run({"TB / transparent Class A receive window", Op::Bidi, 0, payload, 6});
  run({"MEDOA / enqueue detach", Op::OtaDetach});
  run({"MPCS / detach request flag", Op::Pending, 0x10});
  run({"MTRX / send enqueued detach", Op::Message, 0, payload, 0});
  run({"MAI / state after unconfirmed OTA detach", Op::AttachInfo, -1});
  run({"MDLO / local detach cleanup", Op::LocalDetach});
  run({"MAI / detached state", Op::AttachInfo});
  run({"MCFGF / enable IPv6 while detached", Op::Config, 1});
  run({"MCFGF / restore no IPv6", Op::Config, 0});
  if (!stopped) {
    if (reserveNonce()) {
      run({"MTAOA / fresh nonce, no gateway", Op::OtaAttach, 0, nonce, 8});
      run({"MAI / OTA result (success not expected)", Op::AttachInfo, -1});
      run({"MDLO / OTA state cleanup", Op::LocalDetach});
    } else {
      notRun += 3;
      log("NOT_RUN name=MTAOA reason=cannot_reserve_fresh_nonce_EEPROM_unavailable_or_full\r\n");
      log("NOT_RUN name=MAI_after_OTA reason=OTA_not_attempted\r\n");
      log("NOT_RUN name=MDLO_after_OTA reason=OTA_not_attempted\r\n");
    }
  } else {
    notRun += 3;
    log("NOT_RUN name=MTAOA reason=suite_stopped\r\n");
    log("NOT_RUN name=MAI_after_OTA reason=suite_stopped\r\n");
    log("NOT_RUN name=MDLO_after_OTA reason=suite_stopped\r\n");
  }
  summary();
}

void runAtFinalTest(bool bootloader) {
  stopped = logLost = capturing = false;
  desynchronized = false;
  log("\r\nMANUAL terminal command; hardware reset/power cycle may be needed afterwards.\r\n");
  run({bootloader ? "SBTL / manual final test" : "PD / manual final test",
       bootloader ? Op::Bootloader : Op::PowerDown, 0, nullptr, 0, Expect::NoReply});
}

bool atTestLogInterrupted() { return logLost; }
