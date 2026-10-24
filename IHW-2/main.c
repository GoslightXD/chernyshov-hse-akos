#include "common.h"

int main(int argc, char **argv) {
  int rc = config_init(argc, argv);
  if (rc >= 0) {
    flush_out();
    return rc;
  }
  log_open();
  rc = sim_run();
  flush_out();
  log_close();
  return rc;
}