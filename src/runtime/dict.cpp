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

#include "runtime/dict.h"

#include "codegen/compvars.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

Box* dictRepr(BoxedDict* self) {
    std::vector<char> chars;
    chars.push_back('{');
    bool first = true;
    for (const auto& p : self->d) {
        if (!first) {
            chars.push_back(',');
            chars.push_back(' ');
        }
        first = false;

        BoxedString* k = static_cast<BoxedString*>(repr(p.first));
        BoxedString* v = static_cast<BoxedString*>(repr(p.second));
        chars.insert(chars.end(), k->s.begin(), k->s.end());
        chars.push_back(':');
        chars.push_back(' ');
        chars.insert(chars.end(), v->s.begin(), v->s.end());
    }
    chars.push_back('}');
    return boxString(std::string(chars.begin(), chars.end()));
}

Box* dictItems(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();

    for (const auto& p : self->d) {
        BoxedTuple::GCVector elts;
        elts.push_back(p.first);
        elts.push_back(p.second);
        BoxedTuple* t = new BoxedTuple(std::move(elts));
        listAppendInternal(rtn, t);
    }

    return rtn;
}

Box* dictValues(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto& p : self->d) {
        listAppendInternal(rtn, p.second);
    }
    return rtn;
}

Box* dictKeys(BoxedDict* self) {
    BoxedList* rtn = new BoxedList();
    for (const auto& p : self->d) {
        listAppendInternal(rtn, p.first);
    }
    return rtn;
}

Box* dictLen(BoxedDict* self) {
    assert(self->cls == dict_cls);
    return boxInt(self->d.size());
}

Box* dictGetitem(BoxedDict* self, Box* k) {
    assert(self->cls == dict_cls);

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        BoxedString* s = reprOrNull(k);

        if (s)
            raiseExcHelper(KeyError, "%s", s->s.c_str());
        else
            raiseExcHelper(KeyError, "");
    }

    Box* pos = self->d[k];

    return pos;
}

Box* dictSetitem(BoxedDict* self, Box* k, Box* v) {
    // printf("Starting setitem\n");
    Box*& pos = self->d[k];
    // printf("Got the pos\n");

    if (pos != NULL) {
        pos = v;
    } else {
        pos = v;
    }

    return None;
}

Box* dictPop(BoxedDict* self, Box* k, Box* d) {
    assert(self->cls == dict_cls);

    auto it = self->d.find(k);
    if (it == self->d.end()) {
        if (d)
            return d;

        BoxedString* s = reprOrNull(k);

        if (s)
            raiseExcHelper(KeyError, "%s", s->s.c_str());
        else
            raiseExcHelper(KeyError, "");
    }

    Box* rtn = it->second;
    self->d.erase(it);
    return rtn;
}

Box* dictGet(BoxedDict* self, Box* k, Box* d) {
    assert(self->cls == dict_cls);

    auto it = self->d.find(k);
    if (it == self->d.end())
        return d;

    return it->second;
}

Box* dictSetdefault(BoxedDict* self, Box* k, Box* v) {
    assert(self->cls == dict_cls);

    auto it = self->d.find(k);
    if (it != self->d.end())
        return it->second;

    self->d.insert(it, std::make_pair(k, v));
    return v;
}

Box* dictContains(BoxedDict* self, Box* k) {
    assert(self->cls == dict_cls);
    return boxBool(self->d.count(k) != 0);
}

extern "C" Box* dictNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "dict.__new__(X): X is not a type object (%s)", getTypeName(_cls)->c_str());

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, dict_cls))
        raiseExcHelper(TypeError, "dict.__new__(%s): %s is not a subtype of dict", getNameOfClass(cls)->c_str(),
                       getNameOfClass(cls)->c_str());

    RELEASE_ASSERT(cls == dict_cls, "");

    int args_sz = args->elts.size();
    int kwargs_sz = kwargs->d.size();
    BoxedDict* r = new BoxedDict();

    // CPython accepts a single positional and keyword arguments, in any combination
    if (args_sz > 1)
        raiseExcHelper(TypeError, "dict expected at most 1 arguments, got %d", args_sz);

    // handle positional argument first as iterable
    if (args_sz == 1) {
        int idx = 0;
        // raises if not iterable
        llvm::iterator_range<BoxIterator> range = args->elts[0]->pyElements();
        for (BoxIterator it1 = range.begin(); it1 != range.end(); ++it1, idx++) {

            Box* element = *it1;

            // should this check subclasses? anyway to check if something is iterable...
            if (element->cls == list_cls) {
                BoxedList* list = static_cast<BoxedList*>(element);
                if (list->size != 2)
                    raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %d; 2 is required",
                                   idx, list->size);

                r->d[list->elts->elts[0]] = list->elts->elts[1];
            } else if (element->cls == tuple_cls) {
                BoxedTuple* tuple = static_cast<BoxedTuple*>(element);
                if (tuple->elts.size() != 2)
                    raiseExcHelper(ValueError, "dictionary update sequence element #%d has length %d; 2 is required",
                                   idx, tuple->elts.size());

                r->d[tuple->elts[0]] = tuple->elts[1];
            } else
                raiseExcHelper(TypeError, "cannot convert dictionary update sequence element #%d to a sequence", idx);
        }
    }

    // handle keyword arguments by merging (possibly over positional entries per CPy)
    assert(kwargs->cls == dict_cls);

    for (const auto& p : kwargs->d)
        r->d[p.first] = p.second;

    return r;
}

BoxedClass* dict_iterator_cls = NULL;
extern "C" void dictIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);

    BoxedDictIterator* it = static_cast<BoxedDictIterator*>(b);
    v->visit(it->d);
}

void setupDict() {
    dict_iterator_cls = new BoxedClass(object_cls, &dictIteratorGCHandler, 0, sizeof(BoxedDict), false);

    dict_cls->giveAttr("__name__", boxStrConstant("dict"));
    dict_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)dictLen, BOXED_INT, 1)));
    dict_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)dictNew, UNKNOWN, 1, 0, true, true)));
    // dict_cls->giveAttr("__init__", new BoxedFunction(boxRTFunction((void*)dictInit, NULL, 1)));
    dict_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)dictRepr, STR, 1)));
    dict_cls->giveAttr("__str__", dict_cls->getattr("__repr__"));

    dict_cls->giveAttr("__iter__",
                       new BoxedFunction(boxRTFunction((void*)dictIterKeys, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("items", new BoxedFunction(boxRTFunction((void*)dictItems, LIST, 1)));
    dict_cls->giveAttr("iteritems",
                       new BoxedFunction(boxRTFunction((void*)dictIterItems, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("values", new BoxedFunction(boxRTFunction((void*)dictValues, LIST, 1)));
    dict_cls->giveAttr("itervalues",
                       new BoxedFunction(boxRTFunction((void*)dictIterValues, typeFromClass(dict_iterator_cls), 1)));

    dict_cls->giveAttr("keys", new BoxedFunction(boxRTFunction((void*)dictKeys, LIST, 1)));
    dict_cls->giveAttr("iterkeys", dict_cls->getattr("__iter__"));

    dict_cls->giveAttr("pop", new BoxedFunction(boxRTFunction((void*)dictPop, UNKNOWN, 3, 1, false, false), { NULL }));

    dict_cls->giveAttr("get", new BoxedFunction(boxRTFunction((void*)dictGet, UNKNOWN, 3, 1, false, false), { None }));

    dict_cls->giveAttr("setdefault",
                       new BoxedFunction(boxRTFunction((void*)dictSetdefault, UNKNOWN, 3, 1, false, false), { None }));

    dict_cls->giveAttr("__getitem__", new BoxedFunction(boxRTFunction((void*)dictGetitem, UNKNOWN, 2)));
    dict_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)dictSetitem, NONE, 3)));
    dict_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)dictContains, BOXED_BOOL, 2)));

    dict_cls->freeze();

    gc::registerStaticRootObj(dict_iterator_cls);
    dict_iterator_cls->giveAttr("__name__", boxStrConstant("dictiterator"));

    CLFunction* hasnext = boxRTFunction((void*)dictIterHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)dictIterHasnext, BOXED_BOOL);
    dict_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    dict_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)dictIterIter, typeFromClass(dict_iterator_cls), 1)));
    dict_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)dictIterNext, UNKNOWN, 1)));

    dict_iterator_cls->freeze();
}

void teardownDict() {
}
}
