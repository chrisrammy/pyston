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

#ifndef PYSTON_GC_COLLECTOR_H
#define PYSTON_GC_COLLECTOR_H

#include <vector>

#include "core/types.h"

namespace pyston {
namespace gc {

class TraceStack {
private:
    std::vector<void*> v;

public:
    void pushall(void* const* start, void* const* end) { v.insert(v.end(), start, end); }

    void push(void* p) { v.push_back(p); }

    int size() { return v.size(); }

    void reserve(int num) { v.reserve(num + v.size()); }

    void* pop() {
        if (v.size()) {
            void* r = v.back();
            v.pop_back();
            return r;
        }
        return NULL;
    }
};

class TraceStackGCVisitor : public GCVisitor {
private:
    bool isValid(void* p);

public:
    TraceStack* stack;
    TraceStackGCVisitor(TraceStack* stack) : stack(stack) {}

    // These all work on *user* pointers, ie pointers to the user_data section of GCAllocations
    void visit(void* p) override;
    void visitRange(void* const* start, void* const* end) override;
    void visitPotential(void* p) override;
    void visitPotentialRange(void* const* start, void* const* end) override;
};

// Mark this gc-allocated object as being a root, even if there are no visible references to it.
// (Note: this marks the gc allocation itself, not the pointer that points to one.  For that, use
// a StaticRootHandle or registerStaticRootMemory)
void registerStaticRootObj(void* root_obj);
// Register a non-gc region of memory (such as statically-allocated memory) as a source of potential
// GC roots.
void registerStaticRootMemory(void* start, void* end);
void runCollection();

// If you want to have a static root "location" where multiple values could be stored, use this:
class StaticRootHandle {
public:
    Box* value;

    StaticRootHandle();
    ~StaticRootHandle();

    void operator=(Box* b) { value = b; }

    operator Box*() { return value; }
};
}
}

#endif
