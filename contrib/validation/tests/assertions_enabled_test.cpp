// SPDX-License-Identifier: MIT
#include <cassert>
#ifdef NDEBUG
#error Validation must keep assertions enabled in every configuration.
#endif
int main() {
  bool evaluated = false;
  assert((evaluated = true));
  return evaluated ? 0 : 1;
}
