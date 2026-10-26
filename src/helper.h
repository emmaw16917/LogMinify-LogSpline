#ifndef LOGMINIFY_HELPER_H
#define LOGMINIFY_HELPER_H

#include <cstdlib>
#include <iostream>

// Prints an unrecoverable configuration error and exits.
#define COUT_N_EXIT(msg)                   \
  do {                                     \
    std::cout << msg << std::endl;         \
    std::abort();                          \
  } while (0)

#endif  // LOGMINIFY_HELPER_H
