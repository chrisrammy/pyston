// Copyright (c) 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "gc/collector.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "codegen/codegen.h"
#include "core/common.h"
#include "core/threading.h"
#include "core/types.h"
#include "core/util.h"
#include "gc/heap.h"
#include "gc/root_finder.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

//#undef VERBOSITY
//#define VERBOSITY(x) 2

namespace pyston {
namespace gc {

static TraceStack roots;
void registerStaticRootObj(void* obj) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));
    roots.push(obj);
}

std::vector<std::pair<void*, void*> > static_root_memory;
void registerStaticRootMemory(void* start, void* end) {
    assert(start < end);

    // While these aren't necessary to work correctly, they are the anticipated use case:
    assert(global_heap.getAllocationFromInteriorPointer(start) == NULL);
    assert(global_heap.getAllocationFromInteriorPointer(end) == NULL);

    static_root_memory.push_back(std::make_pair(start, end));
}

static std::unordered_set<StaticRootHandle*>* getRootHandles() {
    static std::unordered_set<StaticRootHandle*> root_handles;
    return &root_handles;
}

StaticRootHandle::StaticRootHandle() {
    getRootHandles()->insert(this);
}
StaticRootHandle::~StaticRootHandle() {
    getRootHandles()->erase(this);
}



bool TraceStackGCVisitor::isValid(void* p) {
    return global_heap.getAllocationFromInteriorPointer(p) != NULL;
}

void TraceStackGCVisitor::visit(void* p) {
    assert(global_heap.getAllocationFromInteriorPointer(p)->user_data == p);
    stack->push(p);
}

void TraceStackGCVisitor::visitRange(void* const* start, void* const* end) {
#ifndef NDEBUG
    void* const* cur = start;
    while (cur < end) {
        assert(isValid(*cur));
        cur++;
    }
#endif
    stack->pushall(start, end);
}

void TraceStackGCVisitor::visitPotential(void* p) {
    GCAllocation* a = global_heap.getAllocationFromInteriorPointer(p);
    if (a) {
        visit(a->user_data);
    }
}

void TraceStackGCVisitor::visitPotentialRange(void* const* start, void* const* end) {
    while (start < end) {
        visitPotential(*start);
        start++;
    }
}

static void markPhase() {
#ifndef NVALGRIND
    // Have valgrind close its eyes while we do the conservative stack and data scanning,
    // since we'll be looking at potentially-uninitialized values:
    VALGRIND_DISABLE_ERROR_REPORTING;
#endif

    TraceStack stack(roots);
    collectStackRoots(&stack);

    TraceStackGCVisitor visitor(&stack);

    for (const auto& p : static_root_memory) {
        visitor.visitPotentialRange((void**)p.first, (void**)p.second);
    }

    for (auto h : *getRootHandles()) {
        visitor.visitPotential(h->value);
    }

    // if (VERBOSITY()) printf("Found %d roots\n", stack.size());
    while (void* p = stack.pop()) {
        assert(((intptr_t)p) % 8 == 0);
        GCAllocation* al = GCAllocation::fromUserData(p);

        if (isMarked(al)) {
            continue;
        }

        // printf("Marking + scanning %p\n", p);

        setMark(al);

        GCKind kind_id = al->kind_id;
        if (kind_id == GCKind::UNTRACKED) {
            continue;
        } else if (kind_id == GCKind::CONSERVATIVE) {
            uint32_t bytes = al->kind_data;
            visitor.visitPotentialRange((void**)p, (void**)((char*)p + bytes));
        } else if (kind_id == GCKind::PYTHON) {
            Box* b = reinterpret_cast<Box*>(p);
            BoxedClass* cls = b->cls;

            if (cls) {
                // The cls can be NULL since we use 'new' to construct them.
                // An arbitrary amount of stuff can happen between the 'new' and
                // the call to the constructor (ie the args get evaluated), which
                // can trigger a collection.
                ASSERT(cls->gc_visit, "%s", getTypeName(b)->c_str());
                cls->gc_visit(&visitor, b);
            }
        } else {
            RELEASE_ASSERT(0, "Unhandled kind: %d", (int)kind_id);
        }
    }

#ifndef NVALGRIND
    VALGRIND_ENABLE_ERROR_REPORTING;
#endif
}

static void sweepPhase() {
    global_heap.freeUnmarked();
}

static int ncollections = 0;
void runCollection() {
    static StatCounter sc("gc_collections");
    sc.log();

    ncollections++;

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d\n", ncollections);

    Timer _t("collecting", /*min_usec=*/10000);

    markPhase();
    sweepPhase();
    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    static StatCounter sc_us("gc_collections_us");
    sc_us.log(us);
}

} // namespace gc
} // namespace pyston
