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

#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>

#include "Python.h"

#include "codegen/compvars.h"
#include "core/threading.h"
#include "core/types.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"

namespace pyston {

// A dictionary-like wrapper around the attributes array.
// Not sure if this will be enough to satisfy users who expect __dict__
// or PyModule_GetDict to return real dicts.
BoxedClass* attrwrapper_cls;
class AttrWrapper : public Box {
private:
    Box* b;

public:
    AttrWrapper(Box* b) : Box(attrwrapper_cls), b(b) {}

    static void gcHandler(GCVisitor* v, Box* b) {
        boxGCHandler(v, b);

        AttrWrapper* aw = (AttrWrapper*)b;
        v->visit(aw->b);
    }

    static Box* setitem(Box* _self, Box* _key, Box* value) {
        assert(_self->cls == attrwrapper_cls);
        AttrWrapper* self = static_cast<AttrWrapper*>(_self);

        RELEASE_ASSERT(_key->cls == str_cls, "");
        BoxedString* key = static_cast<BoxedString*>(_key);
        self->b->setattr(key->s, value, NULL);

        return None;
    }
};

BoxedClass* method_cls;
class BoxedMethodDescriptor : public Box {
public:
    PyMethodDef* method;

    BoxedMethodDescriptor(PyMethodDef* method) : Box(method_cls), method(method) {}

    static Box* __call__(BoxedMethodDescriptor* self, Box* obj, BoxedTuple* varargs, Box** _args) {
        BoxedDict* kwargs = static_cast<BoxedDict*>(_args[0]);

        assert(self->cls == method_cls);
        assert(varargs->cls == tuple_cls);
        assert(kwargs->cls == dict_cls);

        threading::GLPromoteRegion _gil_lock;

        int ml_flags = self->method->ml_flags;
        Box* rtn;
        if (ml_flags == METH_NOARGS) {
            assert(varargs->elts.size() == 0);
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->method->ml_meth(obj, NULL);
        } else if (ml_flags == METH_VARARGS) {
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->method->ml_meth(obj, varargs);
        } else {
            RELEASE_ASSERT(0, "0x%x", ml_flags);
        }
        assert(rtn);
        return rtn;
    }
};

extern "C" PyObject* PyModule_GetDict(PyObject* _m) {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    return new AttrWrapper(m);
}

extern "C" int PyModule_AddIntConstant(PyObject* _m, const char* name, long value) {
    BoxedModule* m = static_cast<BoxedModule*>(_m);
    assert(m->cls == module_cls);

    m->setattr(name, boxInt(value), NULL);
    return 0;
}

extern "C" PyObject* PyDict_New() {
    return new BoxedDict();
}

extern "C" PyObject* PyString_FromString(const char* s) {
    return boxStrConstant(s);
}

extern "C" PyObject* PyString_FromStringAndSize(const char* s, ssize_t n) {
    if (s == NULL)
        return boxString(std::string(n, '\x00'));
    return boxStrConstantSize(s, n);
}

extern "C" char* PyString_AsString(PyObject* o) {
    assert(o->cls == str_cls);

    // TODO this is very brittle, since
    // - you are very much not supposed to change the data, and
    // - the pointer doesn't have great longevity guarantees
    // To satisfy this API we might have to change the string representation?
    printf("Warning: PyString_AsString() currently has risky behavior\n");
    return const_cast<char*>(static_cast<BoxedString*>(o)->s.data());
}

extern "C" Py_ssize_t PyString_Size(PyObject* s) {
    RELEASE_ASSERT(s->cls == str_cls, "");
    return static_cast<BoxedString*>(s)->s.size();
}

extern "C" PyObject* PyInt_FromLong(long n) {
    return boxInt(n);
}

extern "C" int PyDict_SetItem(PyObject* mp, PyObject* _key, PyObject* _item) {
    Box* b = static_cast<Box*>(mp);
    Box* key = static_cast<Box*>(_key);
    Box* item = static_cast<Box*>(_item);

    static std::string setitem_str("__setitem__");
    Box* r;
    try {
        // TODO should demote GIL?
        r = callattrInternal(b, &setitem_str, CLASS_ONLY, NULL, ArgPassSpec(2), key, item, NULL, NULL, NULL);
    } catch (Box* b) {
        fprintf(stderr, "Error: uncaught error would be propagated to C code!\n");
        abort();
    }

    RELEASE_ASSERT(r, "");
    return 0;
}

extern "C" int PyDict_SetItemString(PyObject* mp, const char* key, PyObject* item) {
    return PyDict_SetItem(mp, boxStrConstant(key), item);
}


BoxedClass* capifunc_cls;
class BoxedCApiFunction : public Box {
private:
    int ml_flags;
    Box* passthrough;
    const char* name;
    PyCFunction func;

public:
    BoxedCApiFunction(int ml_flags, Box* passthrough, const char* name, PyCFunction func)
        : Box(capifunc_cls), ml_flags(ml_flags), passthrough(passthrough), name(name), func(func) {}

    static BoxedString* __repr__(BoxedCApiFunction* self) {
        assert(self->cls == capifunc_cls);
        return boxStrConstant(self->name);
    }

    static Box* __call__(BoxedCApiFunction* self, BoxedTuple* varargs, BoxedDict* kwargs) {
        assert(self->cls == capifunc_cls);
        assert(varargs->cls == tuple_cls);
        assert(kwargs->cls == dict_cls);

        threading::GLPromoteRegion _gil_lock;

        Box* rtn;
        if (self->ml_flags == METH_VARARGS) {
            assert(kwargs->d.size() == 0);
            rtn = (Box*)self->func(self->passthrough, varargs);
        } else if (self->ml_flags == (METH_VARARGS | METH_KEYWORDS)) {
            rtn = (Box*)((PyCFunctionWithKeywords)self->func)(self->passthrough, varargs, kwargs);
        } else {
            RELEASE_ASSERT(0, "0x%x", self->ml_flags);
        }
        assert(rtn);
        return rtn;
    }
};

extern "C" PyObject* Py_InitModule4(const char* name, PyMethodDef* methods, const char* doc, PyObject* self,
                                    int apiver) {
    BoxedModule* module = createModule(name, "__builtin__");

    Box* passthrough = static_cast<Box*>(self);
    if (!passthrough)
        passthrough = None;

    while (methods->ml_name) {
        if (VERBOSITY())
            printf("Loading method %s\n", methods->ml_name);

        assert((methods->ml_flags & (~(METH_VARARGS | METH_KEYWORDS))) == 0);
        module->giveAttr(methods->ml_name,
                         new BoxedCApiFunction(methods->ml_flags, passthrough, methods->ml_name, methods->ml_meth));

        methods++;
    }

    if (doc) {
        module->setattr("__doc__", boxStrConstant(doc), NULL);
    }

    return module;
}

extern "C" void conservativeGCHandler(GCVisitor* v, Box* b) {
    v->visitPotentialRange((void* const*)b, (void* const*)((char*)b + b->cls->tp_basicsize));
}

extern "C" int PyType_Ready(PyTypeObject* cls) {
    gc::registerStaticRootMemory(cls, cls + 1);

    // unhandled fields:
    RELEASE_ASSERT(cls->tp_print == NULL, "");
    RELEASE_ASSERT(cls->tp_getattr == NULL, "");
    RELEASE_ASSERT(cls->tp_setattr == NULL, "");
    RELEASE_ASSERT(cls->tp_compare == NULL, "");
    RELEASE_ASSERT(cls->tp_repr == NULL, "");
    RELEASE_ASSERT(cls->tp_as_number == NULL, "");
    RELEASE_ASSERT(cls->tp_as_sequence == NULL, "");
    RELEASE_ASSERT(cls->tp_as_mapping == NULL, "");
    RELEASE_ASSERT(cls->tp_hash == NULL, "");
    RELEASE_ASSERT(cls->tp_call == NULL, "");
    RELEASE_ASSERT(cls->tp_str == NULL, "");
    RELEASE_ASSERT(cls->tp_getattro == NULL, "");
    RELEASE_ASSERT(cls->tp_setattro == NULL, "");
    RELEASE_ASSERT(cls->tp_as_buffer == NULL, "");
    RELEASE_ASSERT(cls->tp_flags == Py_TPFLAGS_DEFAULT, "");
    RELEASE_ASSERT(cls->tp_traverse == NULL, "");
    RELEASE_ASSERT(cls->tp_clear == NULL, "");
    RELEASE_ASSERT(cls->tp_richcompare == NULL, "");
    RELEASE_ASSERT(cls->tp_weaklistoffset == 0, "");
    RELEASE_ASSERT(cls->tp_iter == NULL, "");
    RELEASE_ASSERT(cls->tp_iternext == NULL, "");
    RELEASE_ASSERT(cls->tp_members == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_dict == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_get == NULL, "");
    RELEASE_ASSERT(cls->tp_descr_set == NULL, "");
    RELEASE_ASSERT(cls->tp_init == NULL, "");
    RELEASE_ASSERT(cls->tp_alloc == NULL, "");
    RELEASE_ASSERT(cls->tp_new == NULL, "");
    RELEASE_ASSERT(cls->tp_free == NULL, "");
    RELEASE_ASSERT(cls->tp_is_gc == NULL, "");
    RELEASE_ASSERT(cls->tp_base == NULL, "");
    RELEASE_ASSERT(cls->tp_mro == NULL, "");
    RELEASE_ASSERT(cls->tp_cache == NULL, "");
    RELEASE_ASSERT(cls->tp_subclasses == NULL, "");
    RELEASE_ASSERT(cls->tp_weaklist == NULL, "");
    RELEASE_ASSERT(cls->tp_del == NULL, "");
    RELEASE_ASSERT(cls->tp_version_tag == 0, "");

#define INITIALIZE(a) new (&(a)) decltype(a)
    INITIALIZE(cls->attrs);
    INITIALIZE(cls->dependent_icgetattrs);
#undef INITIALIZE

    assert(cls->tp_name);
    cls->giveAttr("__name__", boxStrConstant(cls->tp_name));
    // tp_name
    // tp_basicsize, tp_itemsize
    // tp_doc

    if (cls->tp_methods) {
        PyMethodDef* method = cls->tp_methods;
        while (method->ml_name) {
            auto desc = new BoxedMethodDescriptor(method);
            cls->giveAttr(method->ml_name, desc);
            method++;
        }
    }

    if (cls->tp_getset) {
        if (VERBOSITY())
            printf("warning: ignoring tp_getset for now\n");
    }

    cls->base = object_cls;

    cls->gc_visit = &conservativeGCHandler;

    // TODO not sure how we can handle extension types that manually
    // specify a dict...
    RELEASE_ASSERT(cls->tp_dictoffset == 0, "");
    // this should get automatically initialized to 0 on this path:
    assert(cls->attrs_offset == 0);

    return 0;
}

extern "C" PyObject* Py_BuildValue(const char* arg0, ...) {
    assert(*arg0 == '\0');
    return None;
}

// copied from CPython's getargs.c:
extern "C" int PyBuffer_FillInfo(Py_buffer* view, PyObject* obj, void* buf, Py_ssize_t len, int readonly, int flags) {
    if (view == NULL)
        return 0;
    if (((flags & PyBUF_WRITABLE) == PyBUF_WRITABLE) && (readonly == 1)) {
        // Don't support PyErr_SetString yet:
        assert(0);
        // PyErr_SetString(PyExc_BufferError, "Object is not writable.");
        // return -1;
    }

    view->obj = obj;
    if (obj)
        Py_INCREF(obj);
    view->buf = buf;
    view->len = len;
    view->readonly = readonly;
    view->itemsize = 1;
    view->format = NULL;
    if ((flags & PyBUF_FORMAT) == PyBUF_FORMAT)
        view->format = "B";
    view->ndim = 1;
    view->shape = NULL;
    if ((flags & PyBUF_ND) == PyBUF_ND)
        view->shape = &(view->len);
    view->strides = NULL;
    if ((flags & PyBUF_STRIDES) == PyBUF_STRIDES)
        view->strides = &(view->itemsize);
    view->suboffsets = NULL;
    view->internal = NULL;
    return 0;
}

extern "C" void PyBuffer_Release(Py_buffer* view) {
    if (!view->buf) {
        assert(!view->obj);
        return;
    }

    PyObject* obj = view->obj;
    assert(obj);
    assert(obj->cls == str_cls);
    if (obj && Py_TYPE(obj)->tp_as_buffer && Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer)
        Py_TYPE(obj)->tp_as_buffer->bf_releasebuffer(obj, view);
    Py_XDECREF(obj);
    view->obj = NULL;
}

int vPyArg_ParseTuple(PyObject* _tuple, const char* fmt, va_list ap) {
    RELEASE_ASSERT(_tuple->cls == tuple_cls, "");
    BoxedTuple* tuple = static_cast<BoxedTuple*>(_tuple);

    bool now_optional = false;
    int arg_idx = 0;

    int tuple_size = tuple->elts.size();

    while (char c = *fmt) {
        fmt++;

        if (c == ':') {
            break;
        } else if (c == '|') {
            now_optional = true;
            continue;
        } else {
            if (arg_idx >= tuple_size) {
                RELEASE_ASSERT(now_optional, "");
                break;
            }

            PyObject* arg = tuple->elts[arg_idx];

            switch (c) {
                case 's': {
                    if (*fmt == '*') {
                        Py_buffer* p = (Py_buffer*)va_arg(ap, Py_buffer*);

                        RELEASE_ASSERT(arg->cls == str_cls, "");
                        PyBuffer_FillInfo(p, arg, PyString_AS_STRING(arg), PyString_GET_SIZE(arg), 1, 0);
                        fmt++;
                    } else if (*fmt == ':') {
                        break;
                    } else {
                        RELEASE_ASSERT(0, "");
                    }
                    break;
                }
                case 'O': {
                    PyObject** p = (PyObject**)va_arg(ap, PyObject**);
                    *p = arg;
                    break;
                }
                default:
                    RELEASE_ASSERT(0, "Unhandled format character: '%c'", c);
            }
        }
    }
    return 1;
}

extern "C" int PyArg_ParseTuple(PyObject* _tuple, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    int r = vPyArg_ParseTuple(_tuple, fmt, ap);

    va_end(ap);

    return r;
}

extern "C" int PyArg_ParseTupleAndKeywords(PyObject* args, PyObject* kwargs, const char* format, char** kwlist, ...) {
    assert(kwargs->cls == dict_cls);
    RELEASE_ASSERT(static_cast<BoxedDict*>(kwargs)->d.size() == 0, "");

    va_list ap;
    va_start(ap, kwlist);

    int r = vPyArg_ParseTuple(args, format, ap);

    va_end(ap);

    return r;
}

extern "C" PyObject* _PyObject_New(PyTypeObject* cls) {
    assert(cls->tp_itemsize == 0);
    auto rtn = (PyObject*)gc_alloc(cls->tp_basicsize, gc::GCKind::PYTHON);
    rtn->cls = cls;
    return rtn;
}

extern "C" void PyObject_Free(void* p) {
    gc::gc_free(p);
    ASSERT(0, "I think this is good enough but I'm not sure; should test");
}

extern "C" PyObject* PyErr_Occurred() {
    printf("need to hook exception handling -- make sure errors dont propagate into C code, and error codes get "
           "checked coming out\n");
    return NULL;
}

BoxedModule* importTestExtension() {
    const char* pathname = "../test/test_extension/test.so";
    void* handle = dlopen(pathname, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        exit(1);
    }
    assert(handle);

    void (*init)() = (void (*)())dlsym(handle, "inittest");

    char* error;
    if ((error = dlerror()) != NULL) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }

    assert(init);
    (*init)();

    BoxedDict* sys_modules = getSysModulesDict();
    Box* s = boxStrConstant("test");
    Box* _m = sys_modules->d[s];
    RELEASE_ASSERT(_m, "module failed to initialize properly?");
    assert(_m->cls == module_cls);

    BoxedModule* m = static_cast<BoxedModule*>(_m);
    m->setattr("__file__", boxStrConstant(pathname), NULL);
    m->fn = pathname;
    return m;
}

void setupCAPI() {
    capifunc_cls = new BoxedClass(object_cls, NULL, 0, sizeof(BoxedCApiFunction), false);
    capifunc_cls->giveAttr("__name__", boxStrConstant("capifunc"));

    capifunc_cls->giveAttr("__repr__",
                           new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__repr__, UNKNOWN, 1)));
    capifunc_cls->giveAttr("__str__", capifunc_cls->getattr("__repr__"));

    capifunc_cls->giveAttr(
        "__call__", new BoxedFunction(boxRTFunction((void*)BoxedCApiFunction::__call__, UNKNOWN, 1, 0, true, true)));

    capifunc_cls->freeze();

    attrwrapper_cls = new BoxedClass(object_cls, &AttrWrapper::gcHandler, 0, sizeof(AttrWrapper), false);
    attrwrapper_cls->giveAttr("__name__", boxStrConstant("attrwrapper"));
    attrwrapper_cls->giveAttr("__setitem__", new BoxedFunction(boxRTFunction((void*)AttrWrapper::setitem, UNKNOWN, 3)));
    attrwrapper_cls->freeze();

    method_cls = new BoxedClass(object_cls, NULL, 0, sizeof(BoxedMethodDescriptor), false);
    method_cls->giveAttr("__name__", boxStrConstant("method"));
    method_cls->giveAttr("__call__", new BoxedFunction(boxRTFunction((void*)BoxedMethodDescriptor::__call__, UNKNOWN, 2,
                                                                     0, true, true)));
    method_cls->freeze();
}

void teardownCAPI() {
}
}
