// Host unit tests of the Grow fingerprint protocol (P251_protocol.h).
// g++ -std=c++17 -Wall -Wextra -Werror -Isrc/src/PluginStructs -o test_protocol test/test_protocol.cpp && ./test_protocol

#include "P251_protocol.h"

#include <cstdio>
#include <vector>

using namespace P251_proto;

static int failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, # cond);      \
      ++failures;                                                       \
    }                                                                   \
  } while (0)

static std::vector<uint8_t> build(uint32_t address, uint8_t cmd, std::vector<uint8_t> params) {
  uint8_t buf[MAX_CMD_SIZE];
  const size_t len = buildCommand(buf, address, cmd, params.data(), params.size());

  return std::vector<uint8_t>(buf, buf + len);
}

// Feeds bytes, returns the number of complete packets
static int feedAll(Parser& parser, const std::vector<uint8_t>& bytes) {
  int packets = 0;

  for (uint8_t c : bytes) {
    if (parser.feed(c)) { ++packets; }
  }
  return packets;
}

static void testBuildCommand() {
  // VfyPwd with password 0, as in the datasheet and the Adafruit library
  CHECK((build(DEFAULT_ADDRESS, CMD_VFY_PWD, { 0, 0, 0, 0 }) ==
         std::vector<uint8_t>{ 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x07, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B }));

  // GenImg: no parameters
  CHECK((build(DEFAULT_ADDRESS, CMD_GEN_IMG, {}) ==
         std::vector<uint8_t>{ 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x03, 0x01, 0x00, 0x05 }));

  // Search buffer 1, pages 0..199 (0x00C8), other address
  CHECK((build(0x12345678, CMD_SEARCH, { 1, 0x00, 0x00, 0x00, 0xC8 }) ==
         std::vector<uint8_t>{ 0xEF, 0x01, 0x12, 0x34, 0x56, 0x78, 0x01, 0x00, 0x08, 0x04, 0x01, 0x00, 0x00, 0x00, 0xC8, 0x00, 0xD6 }));

  // Aura LED: breathing, speed 100, purple, forever
  CHECK((build(DEFAULT_ADDRESS, CMD_AURA_LED, { AURA_BREATHING, 100, COLOR_PURPLE, 0 }) ==
         std::vector<uint8_t>{ 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x07, 0x35, 0x01, 0x64, 0x03, 0x00, 0x00, 0xA5 }));

  // Too many parameters
  uint8_t buf[64];
  uint8_t params[MAX_PARAMS + 1]{};
  CHECK(buildCommand(buf, DEFAULT_ADDRESS, CMD_STORE, params, MAX_PARAMS + 1) == 0);
}

static void testParseAck() {
  Parser parser;

  // CODE_OK acknowledge: confirmation code 0
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A }) == 1);
  CHECK(parser.packet().pid == PID_ACK);
  CHECK(parser.packet().address == 0xFFFFFFFF);
  CHECK(parser.packet().size == 1);
  CHECK(parser.packet().code() == CODE_OK);

  // Search result: found page 5, score 100
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x07, 0x00, 0x00, 0x05, 0x00, 0x64, 0x00, 0x77 }) == 1);
  CHECK(parser.packet().code() == CODE_OK);
  CHECK(parser.packet().word(1) == 5);
  CHECK(parser.packet().word(3) == 100);
  CHECK(parser.packet().word(4) == 0); // past the end

  // No finger
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x02, 0x00, 0x0C }) == 1);
  CHECK(parser.packet().code() == CODE_NO_FINGER);
}

static void testResync() {
  Parser parser;

  // Garbage, a lone header byte and a repeated header byte before the packet
  CHECK(feedAll(parser, { 0x00, 0x55, 0xEF, 0x42, 0xEF, 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x09, 0x00, 0x13 }) == 1);
  CHECK(parser.packet().code() == CODE_NOT_FOUND);

  // Bad checksum: dropped and counted, the next packet is fine
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0B }) == 0);
  CHECK(parser.errors() == 1);
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A }) == 1);

  // Length too large (data packet of 128 bytes) and too small: dropped
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x82 }) == 0);
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x01 }) == 0);
  CHECK(parser.errors() == 3);
  CHECK(feedAll(parser, { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x03, 0x00, 0x00, 0x0A }) == 1);
}

static void testSysParams() {
  Parser parser;

  // ReadSysPara: status 0, system id 0, library size 200, security 3, address FFFFFFFF, packet size 2 (128), baud 6 (57600)
  std::vector<uint8_t> pkt = { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x13, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x02, 0x00, 0x06 };
  uint16_t sum = 0;

  for (size_t i = 6; i < pkt.size(); ++i) { sum += pkt[i]; }
  pkt.push_back(sum >> 8);
  pkt.push_back(sum & 0xFF);

  CHECK(feedAll(parser, pkt) == 1);
  CHECK(parser.packet().size == 17);
  CHECK(parser.packet().word(5) == 200); // library size
  CHECK(parser.packet().word(7) == 3);   // security level
  CHECK(parser.packet().word(15) == 6);  // baud / 9600
}

static void testIndexTable() {
  Parser parser;

  // ReadIndexTable page 0: templates 0, 1, 2 and 9 used
  std::vector<uint8_t> pkt = { 0xEF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x23, 0x00, 0x07, 0x02 };

  pkt.resize(9 + 1 + INDEX_PAGE_BYTES, 0);
  uint16_t sum = 0;

  for (size_t i = 6; i < pkt.size(); ++i) { sum += pkt[i]; }
  pkt.push_back(sum >> 8);
  pkt.push_back(sum & 0xFF);

  CHECK(feedAll(parser, pkt) == 1);
  CHECK(parser.packet().size == 1 + INDEX_PAGE_BYTES);
  CHECK(parser.packet().code() == CODE_OK);

  const uint8_t *table = parser.packet().payload + 1;
  CHECK(indexUsed(table, 0));
  CHECK(indexUsed(table, 2));
  CHECK(!indexUsed(table, 3));
  CHECK(indexUsed(table, 9));
  CHECK(indexFirstFree(table, 200) == 3);

  // Full below the capacity
  uint8_t full[INDEX_PAGE_BYTES]{};
  for (size_t i = 0; i < 25; ++i) { full[i] = 0xFF; } // 0..199
  CHECK(indexFirstFree(full, 200) == -1);
  CHECK(indexFirstFree(full, 201) == 200);
  full[24] = 0x7F; // 199 free
  CHECK(indexFirstFree(full, 200) == 199);
}

static void testLookup() {
  CHECK(ledMode("breathing") == AURA_BREATHING);
  CHECK(ledMode("Flash") == AURA_FLASHING);
  CHECK(ledMode("ON") == AURA_ON);
  CHECK(ledMode("off") == AURA_OFF);
  CHECK(ledMode("fadeout") == AURA_FADE_OUT);
  CHECK(ledMode("3") == AURA_ON);
  CHECK(ledMode("0") == 0);
  CHECK(ledMode("7") == 0);
  CHECK(ledMode("of") == 0);
  CHECK(ledMode("offf") == 0);
  CHECK(ledMode("") == 0);
  CHECK(ledMode(nullptr) == 0);

  CHECK(ledColor("red") == COLOR_RED);
  CHECK(ledColor("Purple") == COLOR_PURPLE);
  CHECK(ledColor("white") == COLOR_WHITE);
  CHECK(ledColor("2") == COLOR_BLUE);
  CHECK(ledColor("8") == COLOR_RED - 1);
  CHECK(ledColor("2x") == 0);
  CHECK(ledColor("999") == 0);
  CHECK(ledColor("orange") == 0);
}

int main() {
  testBuildCommand();
  testParseAck();
  testResync();
  testSysParams();
  testIndexTable();
  testLookup();

  if (failures == 0) {
    std::printf("All tests passed\n");
    return 0;
  }
  std::printf("%d test(s) failed\n", failures);
  return 1;
}
