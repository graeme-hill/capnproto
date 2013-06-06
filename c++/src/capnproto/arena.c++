// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define CAPNPROTO_PRIVATE
#include "arena.h"
#include "message.h"
#include <kj/debug.h>
#include <vector>
#include <string.h>
#include <stdio.h>

namespace capnproto {
namespace internal {

Arena::~Arena() {}

void ReadLimiter::unread(WordCount64 amount) {
  // Be careful not to overflow here.  Since ReadLimiter has no thread-safety, it's possible that
  // the limit value was not updated correctly for one or more reads, and therefore unread() could
  // overflow it even if it is only unreading bytes that were acutally read.
  WordCount64 newValue = limit + amount;
  if (newValue > limit) {
    limit = newValue;
  }
}

// =======================================================================================

ReaderArena::ReaderArena(MessageReader* message)
    : message(message),
      readLimiter(message->getOptions().traversalLimitInWords * WORDS),
      segment0(this, SegmentId(0), message->getSegment(0), &readLimiter) {}

ReaderArena::~ReaderArena() {}

SegmentReader* ReaderArena::tryGetSegment(SegmentId id) {
  if (id == SegmentId(0)) {
    if (segment0.getArray() == nullptr) {
      return nullptr;
    } else {
      return &segment0;
    }
  }

  // TODO(someday):  Lock a mutex so that reading is thread-safe.  Take a reader lock during the
  //   first lookup, unlock it before calling getSegment(), then take a writer lock to update the
  //   map.  Bleh, lazy initialization is sad.

  SegmentMap* segments = nullptr;
  KJ_IF_MAYBE(s, moreSegments) {
    auto iter = s->find(id.value);
    if (iter != s->end()) {
      return iter->second.get();
    }
    segments = s;
  }

  kj::ArrayPtr<const word> newSegment = message->getSegment(id.value);
  if (newSegment == nullptr) {
    return nullptr;
  }

  if (moreSegments == nullptr) {
    // OK, the segment exists, so allocate the map.
    auto s = kj::heap<SegmentMap>();
    segments = s;
    moreSegments = mv(s);
  }

  auto segment = kj::heap<SegmentReader>(this, id, newSegment, &readLimiter);
  SegmentReader* result = segment;
  segments->insert(std::make_pair(id.value, mv(segment)));
  return result;
}

void ReaderArena::reportReadLimitReached() {
  KJ_FAIL_REQUIRE("Exceeded message traversal limit.  See capnproto::ReaderOptions.") {
    return;
  }
}

// =======================================================================================

BuilderArena::BuilderArena(MessageBuilder* message)
    : message(message), segment0(nullptr, SegmentId(0), nullptr, nullptr) {}
BuilderArena::~BuilderArena() {}

SegmentBuilder* BuilderArena::getSegment(SegmentId id) {
  // This method is allowed to fail if the segment ID is not valid.
  if (id == SegmentId(0)) {
    return &segment0;
  } else KJ_IF_MAYBE(s, moreSegments) {
    KJ_REQUIRE(id.value - 1 < s->builders.size(), "invalid segment id", id.value);
    return s->builders[id.value - 1].get();
  } else {
    KJ_FAIL_REQUIRE("invalid segment id", id.value);
  }
}

SegmentBuilder* BuilderArena::getSegmentWithAvailable(WordCount minimumAvailable) {
  // TODO(someday):  Mutex-locking?  Do we want to allow people to build different parts of the
  //   same message in different threads?

  if (segment0.getArena() == nullptr) {
    // We're allocating the first segment.
    kj::ArrayPtr<word> ptr = message->allocateSegment(minimumAvailable / WORDS);

    // Re-allocate segment0 in-place.  This is a bit of a hack, but we have not returned any
    // pointers to this segment yet, so it should be fine.
    segment0.~SegmentBuilder();
    return new (&segment0) SegmentBuilder(this, SegmentId(0), ptr, &this->dummyLimiter);
  } else {
    if (segment0.available() >= minimumAvailable) {
      return &segment0;
    }

    MultiSegmentState* segmentState;
    KJ_IF_MAYBE(s, moreSegments) {
      // TODO(perf):  Check for available space in more than just the last segment.  We don't
      //   want this to be O(n), though, so we'll need to maintain some sort of table.  Complicating
      //   matters, we want SegmentBuilders::allocate() to be fast, so we can't update any such
      //   table when allocation actually happens.  Instead, we could have a priority queue based
      //   on the last-known available size, and then re-check the size when we pop segments off it
      //   and shove them to the back of the queue if they have become too small.
      if (s->builders.back()->available() >= minimumAvailable) {
        return s->builders.back().get();
      }
      segmentState = s;
    } else {
      auto newSegmentState = kj::heap<MultiSegmentState>();
      segmentState = newSegmentState;
      moreSegments = kj::mv(newSegmentState);
    }

    kj::Own<SegmentBuilder> newBuilder = kj::heap<SegmentBuilder>(
        this, SegmentId(segmentState->builders.size() + 1),
        message->allocateSegment(minimumAvailable / WORDS), &this->dummyLimiter);
    SegmentBuilder* result = newBuilder.get();
    segmentState->builders.push_back(kj::mv(newBuilder));

    // Keep forOutput the right size so that we don't have to re-allocate during
    // getSegmentsForOutput(), which callers might reasonably expect is a thread-safe method.
    segmentState->forOutput.resize(segmentState->builders.size() + 1);

    return result;
  }
}

kj::ArrayPtr<const kj::ArrayPtr<const word>> BuilderArena::getSegmentsForOutput() {
  // We shouldn't need to lock a mutex here because if this is called multiple times simultaneously,
  // we should only be overwriting the array with the exact same data.  If the number or size of
  // segments is actually changing due to an activity in another thread, then the caller has a
  // problem regardless of locking here.

  KJ_IF_MAYBE(segmentState, moreSegments) {
    KJ_DASSERT(segmentState->forOutput.size() == segmentState->builders.size() + 1,
        "segmentState->forOutput wasn't resized correctly when the last builder was added.",
        segmentState->forOutput.size(), segmentState->builders.size());

    kj::ArrayPtr<kj::ArrayPtr<const word>> result(
        &segmentState->forOutput[0], segmentState->forOutput.size());
    uint i = 0;
    result[i++] = segment0.currentlyAllocated();
    for (auto& builder: segmentState->builders) {
      result[i++] = builder->currentlyAllocated();
    }
    return result;
  } else {
    if (segment0.getArena() == nullptr) {
      // We haven't actually allocated any segments yet.
      return nullptr;
    } else {
      // We have only one segment so far.
      segment0ForOutput = segment0.currentlyAllocated();
      return kj::arrayPtr(&segment0ForOutput, 1);
    }
  }
}

SegmentReader* BuilderArena::tryGetSegment(SegmentId id) {
  if (id == SegmentId(0)) {
    if (segment0.getArena() == nullptr) {
      // We haven't allocated any segments yet.
      return nullptr;
    } else {
      return &segment0;
    }
  } else {
    KJ_IF_MAYBE(segmentState, moreSegments) {
      if (id.value <= segmentState->builders.size()) {
        return segmentState->builders[id.value - 1].get();
      }
    }
    return nullptr;
  }
}

void BuilderArena::reportReadLimitReached() {
  KJ_FAIL_ASSERT(
      "Read limit reached for BuilderArena, but it should have been unlimited.") {
    return;
  }
}

}  // namespace internal
}  // namespace capnproto
