#include "util/secret_shape.h"

#include <print>
#include <string_view>

namespace {

  bool expectSensitive(std::string_view text, bool expected, const char* message) {
    if (secret_shape::looksSensitive(text) != expected) {
      std::println(stderr, "secret_shape_test: {}: expected {} for '{}'", message, expected, text);
      return false;
    }
    return true;
  }

} // namespace

int main() {
  bool ok = true;

  ok = expectSensitive("-----BEGIN OPENSSH PRIVATE KEY-----\nabc", true, "PEM block") && ok;
  ok = expectSensitive("sk-proj-0123456789abcdefghij", true, "token prefix") && ok;
  ok = expectSensitive("sk-proj-0123456789abcdefghij\n", true, "token prefix with trailing newline") && ok;
  ok = expectSensitive("sk-proj-0123456789abcdefghij ", true, "token prefix with trailing space") && ok;
  ok = expectSensitive("ghp_0123456789abcdefghijABCD\r\n", true, "github token with CRLF") && ok;
  ok = expectSensitive("AKIAIOSFODNN7EXAMPLE\n", true, "AWS key id with newline") && ok;
  ok = expectSensitive("https://user:hunter2@example.com/path", true, "URL with password") && ok;
  ok = expectSensitive("postgres://app:s3cret@db:5432/app\n", true, "URL with password and newline") && ok;
  ok = expectSensitive("Ab3dEf6hIj9kLm2nOp5qRs", true, "long mixed run") && ok;

  ok = expectSensitive("https://example.com/path?a=1", false, "plain URL") && ok;
  ok = expectSensitive("https://user@example.com/", false, "URL with user only") && ok;
  ok = expectSensitive("https://example.com/@user:x", false, "colon and at in the path") && ok;
  ok = expectSensitive("hello world", false, "short prose") && ok;
  ok = expectSensitive("this is a longer sentence with 2 numbers", false, "long prose") && ok;
  ok = expectSensitive("sk-short", false, "short token-like word") && ok;
  ok = expectSensitive("xoxo is what I wrote to her", false, "prefix on a short first word") && ok;
  ok = expectSensitive("   \n", false, "whitespace only") && ok;
  ok = expectSensitive("", false, "empty") && ok;

  return ok ? 0 : 1;
}
