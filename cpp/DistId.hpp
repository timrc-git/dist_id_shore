// Copyright 2020, Tim Crowder, All rights reserved.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>

//#include <typeinfo>
#include <stdexcept>

#include "StructArrayStore.hpp"
#include "UDP.hpp"

#ifndef LISTEN_TIME
#  define LISTEN_TIME 3000
#endif
#define MAX_NODES   1024
#define COUNTER_BITS 10
#define MAX_COUNTER  (1<<COUNTER_BITS)

// NOTE: port is hex for "id" :D
//#define MULTICAST_ADDR "224.0.0.152:26980"
#define MULTICAST_ADDR "239.0.0.152:26980"
#define ANY_ADDR "0.0.0.0:0"

int debug = 0;

// Compressed representation of the state of an ID node for serialization.
struct IdNodeState {
  uint64_t timestamp; // millisecond granularity
  uint16_t id;        // node id
  uint16_t port;      // network port of the IdNode
  uint32_t ipaddr;    // raw octet IPV4 address of the IdNode
  uint16_t mode;      // mode for messages: "UP" (server up), "RQ" (request), "HW" (high-water response)

  // Set the mode field from a 2-character string 'm'.
  void SetMode(const char* m) {
    memcpy((char*)&mode, m, 2);
  }
  // returns true if the mode field matches the first two characters of 'm'.
  bool HasMode(const char* m) {
    return 0 == memcmp(m, (const char*)&mode, 2);
  }

  // Set the ipaddr and port fields from 'addr'.
  void SetAddress(const IPAddress& addr) {
    ipaddr = htonl(addr.ip.sin_addr.s_addr);
    port = addr.GetPort();
  }
  // Copy the ipaddr and port fields into 'addr'.
  void GetAddress(IPAddress& addr) {
    addr.ip.sin_addr.s_addr = htonl(ipaddr);
    addr.SetPort(port);
  }
};

// Class which generates "globally" unique 64-bit IDs, 
// and coordinates with peer nodes via Multicast.
// Each running IdNode should have a unique 10 bit 'nodeId'.
class IdNode {

private:
  uint16_t nodeId;      // node identifier (0-1023)
  uint64_t minTimeMs;   // high-water mark timestamp
  uint64_t deltaTimeMs; // offset from monotonic clock to high-water mark
  uint64_t idCounter;   // count of ID-requests since last timestamp update
  IdNodeState state;    // packed node state for storage and transmission
  StructArrayStore<IdNodeState> store;
  MulticastSocket mcSocket;
  IPAddress       mcAddress; 
  UDPSocket       uSocket;
  IPAddress       uAddress;  // local socket address and port
  std::string     uAddressStr;
  bool            initialized;
  bool            hasCollision;

public:

  ////////////////////////////////////////////////////////////
  // public interface

  IdNode() : nodeId(0), minTimeMs(0), deltaTimeMs(0), idCounter(0), initialized(false), hasCollision(false) { }
  ~IdNode() { }

  // Returns true if the node has detected a peer with the same nodeId.
  bool HasCollision() { return hasCollision; }

  // Returns true if the node is fully initialized and ready to return IDs.
  bool IsValid() { return initialized && !HasCollision(); }

  // The whole reason for this class to exist...
  // Returns true if the node is able to generate a unique ID.
  // If so, the id is returned in the (output) parameter 'id'.
  bool GetId(uint64_t& id) {
    // handle any messages
    while (ProcessMulticast(0)) { }
    if (!IsValid()) { return false; }

    if (idCounter >= (MAX_COUNTER-1) || !minTimeMs) {
      if (debug) { fprintf(stderr, "INFO: Update timestamp...\n"); }
      if (!UpdateTimestamp()) {
        fprintf(stderr, "ERROR: Failed to get timestamp!\n");
        return false;
      }
      idCounter = 0;
    }
    id = FieldsToId(minTimeMs, idCounter, nodeId);
    ++idCounter;
    return true;
  }

  // Prepares the node for use.
  // Returns false if it can't be initialized, or a colliding peer is detected.
  bool Initialize(uint16_t node) {
    if (InitNode(node)) {
      return InitNetwork();
    } else {
      fprintf(stderr, "ERROR: InitNode failed! (id:%u)\n", node);
    }
    return false;
  }

  ////////////////////////////////////////////////////////////
  // semi-private interface

  // Converts the separate ID fields into a compound 64-bit timestamp.
  //   'timestamp' - high-water timestamp in milliseconds
  //   'counter'   - a simple counter, must not wrap around before timestamp updates
  //   'node'      - the 10-bit node-id
  static uint64_t FieldsToId(uint64_t timestamp, uint16_t counter, uint16_t node) {
    if (node >= MAX_NODES) { throw std::out_of_range("FieldsToId(): Invalid node id!"); }
    if (counter >= MAX_COUNTER) { throw std::out_of_range("FieldsToId(): Invalid counter value!"); }
    //assert( (node < MAX_NODES) && (counter < MAX_COUNTER) );
    uint64_t id = (timestamp << 20) + (counter << 10) + node;
    return id;
  }

  // Split out an ID to it's fields, This function is just for troubleshooting.
  static void IdToFields(uint64_t &timestamp, uint16_t &counter, uint16_t &node, uint64_t id) {
    node = id & 0x3FF;
    id = id >> 10;
    counter = id & 0x3FF;
    id = id >> 10;
    timestamp = id;
  }

  // Returns minimum (high-water mark) timestamp. This is just for testing.
  uint64_t GetMinTimestamp() { return minTimeMs; }

  // Initializes the (fast) local data of the node.
  //   'node' - a 10-bit identifier for the node.
  bool InitNode(uint16_t node) {
    if (node >= MAX_NODES) {
      fprintf(stderr, "ERROR: Invalid Node-Id %d >= %d\n", node, MAX_NODES);
      return false;
    }
    nodeId = node;

    char buf[64];
    snprintf(buf, 64, "%04d.state", nodeId);
    if (!store.Open(buf, MAX_NODES)) { return false; }
    if (!store.Read(state, nodeId)) {
      fprintf(stderr, "ERROR: Failed to read state for Node-Id %d\n", node);
      return false;
    }
    state.id = node;
    if (state.timestamp == 0) {
      // Never initialized, so write it back...
      store.Write(state, nodeId);
    }

    if (0 != uSocket.Open(ANY_ADDR)) {
      fprintf(stderr, "ERROR: Failed to open UDP socket (%s)\n", ANY_ADDR);
      return false;
    }
    if (0 != mcSocket.Open(MULTICAST_ADDR)) {
      fprintf(stderr, "ERROR: Failed to open multicast socket (%s)\n", MULTICAST_ADDR);
      return false;
    }
    mcSocket.SetTTL(3); // allow limited routing

    mcAddress.SetAddress(MULTICAST_ADDR);
    uSocket.GetAddress(uAddress);
    uAddress.GetString(uAddressStr);
    state.SetAddress(uAddress);

    // startup, request (via multicast) info from peers
    state.SetMode("RQ");
    EmitState(state);
    // start off with stored high-water timestamp (which might be 0)
    AdjustTimetamp(state.timestamp);

    return true;
  }

  // Perform the slower network based initialization of the node. 
  // Waits and processes any messages from peers, to set the high-water timestamp and detect redundant peers.
  bool InitNetwork() {
    // give some time for multicast replies from peers (updates high-water timestamp)
    uint64_t endTs = GetRtTimestampMs() + LISTEN_TIME;
    while (GetRtTimestampMs() < endTs) {
      ProcessMulticast(100);
      if (HasCollision()) { return false; }
    }

    // consider current time as high-water mark
    if (endTs > minTimeMs) { AdjustTimetamp(endTs); }
    initialized = true;

    // announce that we're up
    state.SetMode("UP");
    EmitState(state);

    return true;
  }

  // Send serialized node state object 'state' out to peers.
  bool EmitState(const IdNodeState& state) {
    //return 0 == mcSocket.Write((const char*)&state, sizeof(state));
    return 0 == uSocket.WriteTo(mcAddress, (const char*)&state, sizeof(state));
  }

  // Wait for a message to be available on the multicast socket, 
  // and process it (store state, answer requests, etc.).
  // Returns false if no messages were received
  //   waitMs - maximum milliseconds to wait for a message
  bool ProcessMulticast(int waitMs) {
    if (HasCollision()) { return false; }
    if (!mcSocket.Wait(waitMs)) { return false; }
    char buf[65536];
    IPAddress sourceIp;
    std::string sourceIpStr;
    int read = mcSocket.Read(buf, 65536, sourceIp);
    sourceIp.GetString(sourceIpStr);
    if (debug) { fprintf(stderr, "INFO: Received multicast message (%d bytes from %s).\n", read, sourceIpStr.c_str()); }
    if (sizeof(IdNodeState) != read) {
      if (debug) { fprintf(stderr, "INFO: Received unexpected multicast message (%d bytes).\n", read); }
      return true;
    }

    IdNodeState msgState;
    memcpy(&msgState, buf, sizeof(IdNodeState));
    // handle UP messages (and node collisions)
    if (msgState.HasMode("UP")) {
      if (msgState.id == nodeId) {
        // check if the address matches this node
        //if (uAddress != sourceIp) { // } FIXME uAddress ends up being 0.0.0.0 (any interface) and a real port
        if (uAddress.GetPort() != sourceIp.GetPort()) {
          fprintf(stderr, "ERROR: node-id collision detected (%s vs %s)!\nExiting...\n", uAddressStr.c_str(), sourceIpStr.c_str());
          hasCollision = true;
          return false;
        }
      } else {
        // most recent data from that node, store it
        // FIXME: technically UDP packets can be re-ordered, 
        //   so load the old state, and take the one with max timestamp
        store.Write(msgState, msgState.id);
      }
    }
    // Request from peer for stored state...
    if (msgState.HasMode("RQ")) {
      if (debug) { fprintf(stderr, "INFO: Received 'RQ' multicast message (node %d from %s).\n", msgState.id, sourceIpStr.c_str()); }
      IdNodeState peerState;
      // look it up
      if (!store.Read(peerState, msgState.id)) {
        return true;
      }
      // don't forward un-initialized entries
      if (0 == msgState.timestamp) { return true; }
      // send it out
      if (initialized && msgState.id == nodeId) {
        // as a collision
        peerState.SetMode("UP");
      } else {
        // as a state update
        peerState.SetMode("HW");
      }
      if (debug) { 
        fprintf(stderr, "INFO: Emitting 'HW' multicast message (to node %d from %d).\n", msgState.id, nodeId);
        fprintf(stderr, "INFO:   timestamp %" PRIx64 " vs local %" PRIx64 ".\n", msgState.timestamp, minTimeMs);
      }
      EmitState(peerState);
    }
    // high-water timestamp
    if (msgState.HasMode("HW")) {
      if (debug) { 
        fprintf(stderr, "INFO: Node %u Received 'HW' multicast message (node %d from %s).\n", nodeId, msgState.id, sourceIpStr.c_str());
        fprintf(stderr, "INFO:   timestamp %" PRIx64 " vs local %" PRIx64 ".\n", msgState.timestamp, minTimeMs);
      }
      if (msgState.id == nodeId) {
        // update timestamp/delta
        if (msgState.timestamp > minTimeMs) {
          AdjustTimetamp(msgState.timestamp);
        }
      }
    }

    return true;
  }

  // Sets new high-water timestamp, calculating a new delta from the monotonic time source.
  void AdjustTimetamp(uint64_t timestamp) {
    uint64_t base = GetMonoTimestampMs();
    minTimeMs = timestamp;
    // TODO  assert( base < timestamp );
    deltaTimeMs  = timestamp - base;
    // update the local state store
    state.timestamp = timestamp;
    store.Write(state, nodeId);
  }

  // Returns "Real" time (milliseconds), but subject to "warping" forward and back.
  static uint64_t GetRtTimestampMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = tv.tv_sec*1000;
    now += (tv.tv_usec/1000);
    return now;
  }

  // Returns monotonic time (milliseconds), but with arbitrary origin.
  static uint64_t GetMonoTimestampMs() {
    struct timespec ts;
    // NOTE: CLOCK_MONOTONIC (and BOOTTIME) has an arbitrary offset (origin)
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    //clock_gettime(CLOCK_BOOTTIME, &ts);
    uint64_t now = ts.tv_sec*1000;
    //now += ((int64_t)ts.tv_nsec)/(int64_t)1000000; // strangely, this doesn't work
    now += roundl(ts.tv_nsec/1000000);               // but this does
    return now;
  }

  // Bumps the current timestamp.
  // Returns:
  //    0 - if successfull
  //    1 - on error
  //   -1 - when throttling (delay) is required. 
  int GetCheckedTimestampMs(uint64_t &timeMs) {
    uint64_t now = GetMonoTimestampMs() + deltaTimeMs;
    if (now < timeMs) {
      fprintf(stderr, "ERROR: Non-monotonic clock! (%d)\n", (int)(now-timeMs));
      return -1;
    } else if (now == timeMs) {
      if (debug) { fprintf(stderr, "NOTICE: Request-rate exceeded!\n"); }
      return 1;
    }
    timeMs = now;
    return 0;
  }

  // Bumps the current timestamp with throttling delay.
  // Returns false on error.
  bool UpdateTimestampInner() {
    for (int retry=0; retry<=10; ++retry) {
      if (0 == GetCheckedTimestampMs(minTimeMs)) {
        return true;
      }
      if (debug) { fprintf(stderr, "WARN: Throttling (.1 ms sleep)!\n"); }
      usleep(100);
    }
    return false;
  }

  // Bumps the current timestamp, and serializes it to disk and network.
  bool UpdateTimestamp() {
    if (!UpdateTimestampInner()) {
      fprintf(stderr, "ERROR: Failed to update timestamp! Check date and high-water mark.\n");
      return false;
    }
    //  update stored state 
    state.timestamp = minTimeMs;
    if (!store.Write(state, nodeId)) {
      fprintf(stderr, "ERROR: Failed to write state for Node-Id %d\n", nodeId);
      return false;
    }
    if (debug) { fprintf(stderr, "INFO: emitting MC update...\n"); }
    // emit multicast update
    memcpy(&state.mode, "UP", 2);
    EmitState(state);
    return true;
  }

};


