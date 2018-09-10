// MIT License
//
// Copyright (c) 2018 the Cargo authors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#ifndef CARGO_INCLUDE_LIBCARGO_TYPES_H_
#define CARGO_INCLUDE_LIBCARGO_TYPES_H_

#include <chrono>
#include <limits>
#include <unordered_map>
#include <utility> /* std::pair */

namespace cargo {

/* We use many "logical" numerical types such as node IDs, edge IDs, trip IDs,
 * etc. Unfortunately these types can get "mingled" in the code. For example,
 * consider, where TripId and NodeId are int types:
 *     TripId tid;
 *     NodeId nid;
 *
 * The following assignment is allowed by C++ even though the two are logically
 * different:
 *     nid = tid;
 *     tid = nid;
 *
 * There are other similar issues in type conversion.
 *
 * Google or-tools has a nice template class in int_type.h to prevent these
 * kinds of issues. We can consider using their template IntType class in the
 * future. But for now, using typedefs to at least provide some semantic
 * difference is better than nothing. But we don't have static type-checking.
 */

// "NodeId" type-class
typedef int NodeId;
typedef int OrigId;
typedef int DestId;

// "TripId" type-class
typedef int TripId;
typedef int VehlId;
typedef int CustId;

// "Lon/Lat" type-class
typedef double Lon;
typedef double Lat;

struct Point {
  Lon lng;
  Lat lat;
};

struct BoundingBox {
  Point lower_left;
  Point upper_right;
};

// unit: meters
typedef int    DistInt;
typedef float  DistFlt;
typedef double DistDbl;

/* "SimlTime" type class
 * One SimlTime is one atom of time. Simulation starts at SimlTime = 0.
 * All times (time windows, travel times) are expressed as SimlTime. */
typedef int SimlTime;
typedef int SimlDur;
typedef int ErlyTime;  // time window early
typedef int LateTime;  // time window late

// Meters per second
typedef float Speed;

enum class StopType {
  CustOrig,  // = 0
  CustDest,  // = 1
  VehlOrig,  // = 2
  VehlDest,  // = 3
};

// TODO add a "not appeared" status
enum class CustStatus {
  Waiting,   // = 0
  Onboard,   // = 1
  Arrived,   // = 2
  Canceled,  // = 3
};

enum class VehlStatus {
  Waiting,  // = 0
  Enroute,  // = 1
  Arrived,  // = 2
};

typedef int Load;  // positive=customer, negative=vehicle

typedef std::pair<DistInt, NodeId> Wayp;

typedef size_t RteIdx;
typedef size_t SchIdx;

// Lookup nodes.
typedef std::unordered_map<NodeId, Point> KVNodes;

// Lookup edges.  The key-value store is "undirected"; that is, from-to and
// to-from key combinations both exist in the store. Usage:
//     KVEdges em_;
//     em[from_id][to_id] = weight;
typedef std::unordered_map<NodeId, std::unordered_map<NodeId, DistDbl>> KVEdges;

// Filepath
typedef std::string Filepath;

// Infinity
const int    InfInt = std::numeric_limits<int>::max();
const double InfDbl = std::numeric_limits<double>::infinity();

// Math PI
const double MathPI = 3.141592653589793238462643383279502884L;

// SQLite
typedef int         SqliteReturnCode;
typedef char*       SqliteErrorMessage;
typedef const char* SqliteQuery;

// Chrono
typedef std::chrono::duration<double, std::milli> dur_milli;
typedef std::chrono::milliseconds milli;
typedef std::chrono::high_resolution_clock hiclock;

}  // namespace cargo

#endif  // CARGO_INCLUDE_LIBCARGO_TYPES_H_

