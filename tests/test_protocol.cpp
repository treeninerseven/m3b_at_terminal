#include "TestProtocol.h"
#include <miotyAtClient.h>
#include <cassert>
#include <iostream>
using namespace TestProtocol;
View view(const char *s) { return {reinterpret_cast<const uint8_t *>(s), strlen(s)}; }
int main() {
  static_assert(MIOTYATCLIENT_RETURN_CODE_ATReadFailed == 23, "enum drift");
  static_assert(MIOTYATCLIENT_RETURN_CODE_ATDataSizeOOB == 24, "enum drift");
  static_assert(MIOTYATCLIENT_RETURN_CODE_ATTimeout == 25, "enum drift");
  static_assert(MIOTYATCLIENT_RETURN_CODE_Unsupported == 100, "enum drift");
  assert(result(view("-I:a10\r\n0\r\n")).finalCode == 0);
  assert(result(view("-I:a10\r\n0\r")).finalCode == -1);
  assert(result(view("A_DATA:3\t300D0A\x1A\r\n")).finalCode == -1);
  assert(result(view("2\r\n0\r\n")).finalCode == 2);
  auto r = result(view("-INFO:-4\r\n1\r\n"));
  assert(r.hasInfo && r.info == -4 && mappedCode(r) == MIOTYATCLIENT_RETURN_CODE_MacAlreadyAttached);
  r = result(view("AT!ERR:5\r\n2\r\n"));
  assert(mappedCode(r) == MIOTYATCLIENT_RETURN_CODE_ATDataSizeOOB);
  assert(mappedCode(result(view("100\r\n"))) == 100);
  assert(mappedCode(result(view("-INFO:-100\r\n1\r\n"))) == 100);
  assert(mappedCode(result(view("-MAC_ERR:2\r\n1\r\n"))) == 1);
  int64_t numberValue;
  assert(integer(view("-TXPL:-3\r\n0\r\n"), "TXPL", numberValue) && numberValue == -3);
  assert(!integer(view("+IPR:1200,9600\r\n0\r\n"), "+IPR", numberValue));
  assert(!number(view("12bad"), numberValue));
  assert(!number(view("9223372036854775808"), numberValue));
  assert(!number(view(""), numberValue));
  assert(event(view("0\r\n*Z_ACTIVE:1\r\n*Z_ACTIVE:0\r\n"), "Z_ACTIVE", 0));
  assert(!event(view("0\r\n*Z_ACTIVE:1\r\n"), "Z_ACTIVE", 0));
  View text;
  assert(field(view("-I:Fraunhofer;MixedCase\r\n0\r\n"), "I", text));
  assert(text.equals("Fraunhofer;MixedCase"));
  uint8_t output[4] = {}; size_t length = 99;
  assert(bytes(view("2\t00fF\x1A"), output, 4, length) && length == 2 && output[1] == 255);
  assert(!bytes(view("2\t00FF00"), output, 4, length));
  assert(!bytes(view("2\t0ZFF"), output, 4, length));
  assert(!bytes(view("5\t0000000000"), output, 4, length));
  assert(bytes(view("0\t\x1A"), output, 4, length) && length == 0);
  std::cout << "Independent protocol oracle: all checks passed\n";
}
