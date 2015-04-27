#include "test.hpp"
#include "prime_server.hpp"

namespace {

  void test_separate() {

  }

  void test_delineate() {

  }

}

int main() {
  test::suite suite("netstring");

  suite.test(TEST_CASE(test_separate));

  suite.test(TEST_CASE(test_delineate));

  return suite.tear_down();
}
