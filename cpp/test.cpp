
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <set>
#include <vector>

using namespace std;

// Lower initialization time to speed up tests
#define LISTEN_TIME 500

#include "DistId.hpp"

////////////////////////////////////////////////////////////
// Super minimal test framework

#define NORMAL "\033[0m"
#define RED    "\033[1;31m"
#define GREEN  "\033[1;32m"
#define BLUE   "\033[1;34m"

int testCount = 0;
int failCount = 0;
int prevFailed = 0;

#define TEST_CONDITION(TC)      \
    if (TC) { ++testCount; }    \
    else {                      \
      ++testCount; ++failCount; \
      fprintf(stderr, RED "ERROR: %s:%d Test Failed: (" #TC ")\n" NORMAL, __FILE__, __LINE__); \
    } 

#define TEST_THROW(TC)          \
    try {                       \
      TC;                       \
      ++testCount; ++failCount; \
      fprintf(stderr, RED "ERROR: %s:%d Missing Exception: (" #TC ")\n" NORMAL, __FILE__, __LINE__); \
    } catch (...) {  ++testCount; }

void TEST_BANNER(const char* msg) {
  if (testCount > 0) {
    if (failCount > prevFailed) {
      fprintf(stderr, RED "FAILED.\n" NORMAL);
      prevFailed = failCount;
    } else {
      fprintf(stderr, GREEN "PASSED.\n" NORMAL);
    }
  }
  fprintf(stderr, "---------- Testing %s ----------\n", msg);
}

int TEST_SUMMARY() {
  fprintf(stderr, "============================================================\n");
  fprintf(stderr, GREEN "Tests Run:    %d \n" NORMAL, testCount);
  if (failCount) {
    fprintf(stderr, RED   "Tests Failed: %d \n" NORMAL, failCount);
  } else {
    fprintf(stderr, BLUE  "Tests Failed: %d \n" NORMAL, failCount);
  }
  fprintf(stderr, "============================================================\n");
  return failCount;
}

////////////////////////////////////////////////////////////
// Helper functions

// Pull identifiers from a group of IdNodes and verify uniqueness.
//   nodes - 
bool CheckIdentifiers(vector<IdNode*>& nodes, unsigned idCount, bool monotonic=false, bool canFail=false) {
  set<uint64_t> ids;
  unsigned nodeCount = nodes.size();
  uint64_t lastId = 0;
  unsigned validIds = 0;

  for (unsigned i=0; i<idCount; ++i) {
    uint64_t id;
    unsigned index = i%nodeCount;
    IdNode &curNode = *nodes[index];
    if (curNode.GetId(id)) {
      ++validIds;
      if (ids.find(id) != ids.end()) {
        uint16_t node, counter;
        uint64_t ts;
        curNode.IdToFields(ts, counter, node, id);
        fprintf(stderr, "ERROR: Node %u returned duplicate ID %" PRIx64 " => {t:%" PRIu64 ", c:%u, n:%u} (i=%u) !\n", index, id, ts, counter, node, i);
        return false;
      }
      ids.insert(id);
      if (monotonic && lastId >= id) {
        fprintf(stderr, "ERROR: Node %u returned non-monotonic ID %" PRIx64 " vs %" PRIx64  " (i=%u)!\n", i%nodeCount, id, lastId, i);
        return false;
      }
      lastId = id;
    } else {
      if (!canFail) {
        fprintf(stderr, "ERROR: Node %u failed to return an ID (i=%u)!\n", index, i);
        return false;
      }
    }
  }
  // at least one node should be functioning...
  unsigned expectedIds = idCount/nodeCount;
  if (validIds < expectedIds) {
    fprintf(stderr, "ERROR: Didn't generate minimum number of IDs (%u vs %u)!\n", validIds, expectedIds);
    return false;
  }
  return true;
}

////////////////////////////////////////////////////////////
// actual tests

int main(int argc, char* argv[]) {


  { // Test ID raw construction
    IdNode node;
    uint64_t id1, id2;

    TEST_BANNER("ID Consistency");
      id1 = node.FieldsToId(1234567, 123, 234);
      id2 = node.FieldsToId(1234567, 123, 234);
      TEST_CONDITION(id1 == id2);
    TEST_BANNER("Mutate node (no assumed order)");
      id2 = node.FieldsToId(1234567, 123, 235);
      TEST_CONDITION(id1 != id2);
    TEST_BANNER("Mutate counter (assumed order)");
      id2 = node.FieldsToId(1234567, 124, 234);
      TEST_CONDITION(id1 < id2);
    TEST_BANNER("Mutate timestamp (assumed order)");
      id2 = node.FieldsToId(1234568, 123, 234);
      TEST_CONDITION(id1 < id2);

    TEST_BANNER("Invalid ID fields");
      TEST_THROW(node.FieldsToId(1, 1, 1024));
      TEST_THROW(node.FieldsToId(1, 1024, 1));

    TEST_BANNER("ID field boundary conditions (node)");
      id1 = node.FieldsToId(1234567, 123, 1022);
      id2 = node.FieldsToId(1234567, 123, 1023);
      TEST_CONDITION(id1 < id2);
      id2 = node.FieldsToId(1234567, 123, 0);
      TEST_CONDITION(id1 > id2);

    TEST_BANNER("ID field boundary conditions (counter)");
      id1 = node.FieldsToId(1234567, 1022, 123);
      id2 = node.FieldsToId(1234567, 1023, 123);
      TEST_CONDITION(id1 < id2);
      id2 = node.FieldsToId(1234567, 0, 123);
      TEST_CONDITION(id1 > id2);

  }

  TEST_BANNER("Single Node, normal functioning");
  {
    unsigned idCount = 1000000;
    IdNode node1;
    uint16_t nodeId1 = 123;
    vector<IdNode*> nodes;

    TEST_CONDITION(node1.Initialize(nodeId1));
    nodes.push_back(&node1);
    uint64_t start = node1.GetRtTimestampMs();
    TEST_CONDITION(CheckIdentifiers(nodes, idCount, true));
    uint64_t end = node1.GetRtTimestampMs();
    fprintf(stderr, "Generated %u IDs in %5.3f seconds.\n", idCount, (end-start)/1000.0);
  }

  TEST_BANNER("Peer Nodes, normal functioning");
  {
    unsigned idCount = 1000000;
    IdNode node1;
    uint16_t nodeId1 = 123;
    IdNode node2;
    uint16_t nodeId2 = 234;
    vector<IdNode*> nodes;

    TEST_CONDITION(node1.Initialize(nodeId1));
    nodes.push_back(&node1);
    TEST_CONDITION(node2.Initialize(nodeId2));
    nodes.push_back(&node2);
    TEST_CONDITION(CheckIdentifiers(nodes, idCount, false));
  }

  TEST_BANNER("Peer Nodes, redundant peer should exit");
  {
    // Note: this test will be timing sensitive.
    unsigned idCount = 1000000;
    IdNode node1;
    uint16_t nodeId1 = 123;
    IdNode node2;
    uint16_t nodeId2 = 123;
    vector<IdNode*> nodes;

    // need to get multicast listeners up first
    TEST_CONDITION(node1.InitNode(nodeId1));
    TEST_CONDITION(node2.InitNode(nodeId2));

    bool net1 = node1.InitNetwork();
    bool net2 = node2.InitNetwork();
    // one should be up, the other down
    TEST_CONDITION(net1 != net2);
    nodes.push_back(&node1);
    nodes.push_back(&node2);
    TEST_CONDITION(CheckIdentifiers(nodes, idCount, false, true));
  }

  TEST_BANNER("Node timestamp high-water mark from StructArrayStore");
  {
    uint16_t nodeId1 = 123;
    char stateFilename[64];
    // clean up first
    snprintf(stateFilename, 64, "%04u.state", nodeId1);
    unlink(stateFilename);

    { // prep the StructArrayStore ...
      IdNode node1;
      TEST_CONDITION(node1.Initialize(nodeId1));
    }
    {
      StructArrayStore<IdNodeState> store;
      IdNodeState state;
      IdNode node1;


      TEST_CONDITION(store.Open(stateFilename, MAX_NODES));
      TEST_CONDITION(store.Read(state, nodeId1));
      TEST_CONDITION(state.id == nodeId1);
      TEST_CONDITION(state.timestamp > 0);
      TEST_CONDITION(state.timestamp <= node1.GetRtTimestampMs());

      // Put artificially high ts in StructArrayStore.
      // Set the stored timestamp to 5 seconds in the future.
      state.timestamp = node1.GetRtTimestampMs() + 5*1000;
      TEST_CONDITION(store.Write(state, nodeId1));

      TEST_CONDITION(node1.InitNode(nodeId1));
      TEST_CONDITION(node1.GetMinTimestamp() >= state.timestamp);
    }
  }

  TEST_BANNER("Node timestamp high-water mark from Peer (via Multicast)");
  {
    uint16_t nodeId1 = 123;
    uint16_t nodeId2 = 234;
    char stateFilename[64];
    // clean up first
    snprintf(stateFilename, 64, "%04u.state", nodeId1);
    unlink(stateFilename);

    { // prep two StructArrayStore ...
      IdNode node1;
      TEST_CONDITION(node1.Initialize(nodeId1));
      IdNode node2;
      TEST_CONDITION(node2.Initialize(nodeId2));

      // Query some IDs, to process Multicast chatter
      vector<IdNode*> nodes;
      nodes.push_back(&node1);
      nodes.push_back(&node2);
      TEST_CONDITION(CheckIdentifiers(nodes, 5000, false, true));
    }
    {
      StructArrayStore<IdNodeState> store;
      IdNodeState state;
      IdNode node1;
      IdNode node2;
      uint64_t tmpId;

      // Note: this is node1's store, and node2's entry in it.
      TEST_CONDITION(store.Open(stateFilename, MAX_NODES));
      TEST_CONDITION(store.Read(state, nodeId2));
      TEST_CONDITION(state.id == nodeId2);
      TEST_CONDITION(state.timestamp > 0);
      TEST_CONDITION(state.timestamp <= node2.GetRtTimestampMs());

      // Put artificially high ts in StructArrayStore.
      // Set the stored timestamp to 5 seconds in the future.
      // This will be broadcast to the peer on startup
      state.timestamp = node2.GetRtTimestampMs() + 5*1000;
      TEST_CONDITION(store.Write(state, nodeId2));
      fprintf(stderr, "INFO: Forcing timestamp %" PRIx64 " for node %u.\n", state.timestamp, state.id);

      TEST_CONDITION(node1.Initialize(nodeId1));

      // node2 should get a multicast message from node1 to bump it's timestamp
      TEST_CONDITION(node2.Initialize(nodeId2));
      // Get IDs to process some multicast messages 
      TEST_CONDITION(node1.GetId(tmpId));
      TEST_CONDITION(node2.GetId(tmpId));

      fprintf(stderr, "INFO: got timestamp %" PRIx64 " for node %u.\n", node2.GetMinTimestamp(), nodeId2);
      TEST_CONDITION(node2.GetMinTimestamp() >= state.timestamp);
    }
  }

  return TEST_SUMMARY();
}
