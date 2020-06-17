// Copyright 2020, Tim Crowder, All rights reserved.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string>
#include <type_traits>

// File-based storage for a fixed-size array of uniformly-sized structured data elements.
// Provides functions to individually read and write individual elements.
// New files are zero-padded.
template<typename S> class StructArrayStore {
  // ensure it's a "plain-old-data" type...
  static_assert(std::is_pod<S>::value, "S must be POD");
private:
  int fd;
  unsigned size;
  std::string name;

public:
  StructArrayStore() : fd(-1) { }
  ~StructArrayStore() { Close(); }

  void Close() {
    if (fd >= 0) { close(fd); }
    fd = -1;
  }

  // Open the StructArrayStore for reading and writing.
  //   fname - filename for storage
  //   size  - the number of records to store
  bool Open(const char* fname, unsigned size) {
    name = fname;
    this->size = size;
    fd = open(fname, O_RDWR);
    if (-1 == fd) { // failed to open, try to create it
      fd = open(fname, O_RDWR|O_CREAT, 0664);
      if (-1 == fd) {
        fprintf(stderr, "ERROR: Failed to open/create StructArrayStore '%s' !\n", fname);
        return false;
      }
      // newly created file, pad out to full size...
      char buf[sizeof(S)];
      memset(buf, 0, sizeof(S));
      for (unsigned i=0; i<size; ++i) {
        Write(*(S*)buf, i);
      }
    }
    return true;
  }

  // Reads a single entry from the file by array index.
  // Returns true on success.
  //   'entry' - the data element to fill with data from the file.
  //   'index' - the position to read from.
  bool Read(S &entry, unsigned index) {
    if (index >= size) {
      fprintf(stderr, "ERROR: Invalid StructArrayStore read index (%u vs %u)\n", index, size);
      return false;
    }
    ssize_t ret = pread(fd, (void*)&entry, sizeof(S), sizeof(S)*index);
    return ret == sizeof(S);
  }

  // Writes a single entry to the file by array index.
  // Returns true on success.
  //   'entry' - the data element to write to the file.
  //   'index' - the position to write at.
  bool Write(const S &entry, unsigned index, bool flush=true) {
    if (index >= size) {
      fprintf(stderr, "ERROR: Invalid StructArrayStore write index (%u vs %u)\n", index, size);
      return false;
    }
    // TODO add checksum or duplicate record to catch write-tearing on unclean shutdown
    ssize_t ret = pwrite(fd, (const void*)&entry, sizeof(S), sizeof(S)*index);
    //if (flush) { fsync(fd); }
    return ret == sizeof(S);
  }
};

