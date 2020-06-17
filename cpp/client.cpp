#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "DistId.hpp"


int main(int argc, char* argv[]) {
  IdNode node;
  uint64_t id;
  unsigned idCount = 1000000;

  if (argc < 2) {
    fprintf(stderr, "Missing NodeId argument!\n");
    return 1;
  } else if (argc == 3) {
    idCount = strtol(argv[2], NULL, 10);
  } else if (argc > 3) {
    fprintf(stderr, "Unexpected extra argument!\n");
    return 1;
  }
  // TODO validate it's a number...
  id = strtol(argv[1], NULL, 10);

  if (!node.Initialize(id)) {
    fprintf(stderr,"ERROR: Failed to initialize IdNode properly!\n");
    return 2;
  }
  for (unsigned i=0; i<idCount; ++i) {
    node.GetId(id);
    fprintf(stdout, "%" PRIx64 "\n", id);
  }
}
