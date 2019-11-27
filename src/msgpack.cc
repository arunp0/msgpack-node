#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>
#include <cmath>
#include <iostream>
#include <vector>
#include <stack>
#include <queue>
#include <nan.h>
#include <stdio.h>

using namespace std;
using namespace v8;
using namespace node;

#define SBUF_POOL 50000

// MSC does not support C99 trunc function.
#ifdef _MSC_BUILD
double trunc(double d){ return (d>0) ? floor(d) : ceil(d) ; }
#endif


static Nan::Persistent<FunctionTemplate> msgpack_unpack_template;


// An exception class that wraps a textual message
class MsgpackException {
    public:
        MsgpackException(const char *str) :
            msg(Nan::New<String>(str).ToLocalChecked()) {
        }

        Local<Value> getThrownException() {
            return Nan::TypeError(msg);
        }

    private:
        const Local<String> msg;
};

// A holder for a msgpack_zone object; ensures destruction on scope exit
class MsgpackZone {
    public:
        msgpack_zone _mz;

        MsgpackZone(size_t sz = 1024) {
            msgpack_zone_init(&this->_mz, sz);
        }

        ~MsgpackZone() {
            msgpack_zone_destroy(&this->_mz);
        }
};

struct msgpack_packet {
    Local<Value> v8obj;
    msgpack_object *mo;
    msgpack_zone *mz;
    size_t depth;
};

struct msgpack_packet_s {
    Local<Value> v8val;
    Local<Object> v8obj;
    Local<Array> v8arr;
    msgpack_object *mo;
};

static stack<msgpack_sbuffer *> sbuffers;

#define DBG_PRINT_BUF(buf, name) \
    do { \
        fprintf(stderr, "Buffer %s has %lu bytes:\n", \
            (name), Buffer::Length(buf) \
        ); \
        for (uint32_t i = 0; i * 16 < Buffer::Length(buf); i++) { \
            fprintf(stderr, "  "); \
            for (uint32_t ii = 0; \
                 ii < 16 && (i * 16) + ii < Buffer::Length(buf); \
                 ii++) { \
                fprintf(stderr, "%s%2.2hhx", \
                    (ii > 0 && (ii % 2 == 0)) ? " " : "", \
                    Buffer::Data(buf)[i * 16 + ii] \
                ); \
            } \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

// This will be passed to Buffer::New so that we can manage our own memory.
// In other news, I am unsure what to do with hint, as I've never seen this
// coding pattern before.  For now I have overloaded it to be a void pointer
// to a msgpack_sbuffer.  This let's us push it onto the stack for use later.
static void
_free_sbuf(char *data, void *hint) {
    if (data != NULL && hint != NULL) {
        msgpack_sbuffer *sbuffer = (msgpack_sbuffer *)hint;
        if (sbuffers.size() > SBUF_POOL ||
            sbuffer->alloc > (MSGPACK_SBUFFER_INIT_SIZE * 5)) {
            msgpack_sbuffer_free(sbuffer);
        } else {
            sbuffer->size = 0;
            sbuffers.push(sbuffer);
        }
    }
}


// Convert a V8 object to a MessagePack object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
//
// If a circular reference is detected, an exception is thrown.
static void
v8_to_msgpack(Local<Value> v8obj, msgpack_object *mo, msgpack_zone *mz, size_t depth) {
    stack<msgpack_packet *> msgpack_packets;
    msgpack_packet *newmp = new msgpack_packet();
    newmp->depth = depth;
    newmp->mz = mz;
    newmp->mo = mo;
    newmp->v8obj = v8obj;
    msgpack_packets.push(newmp);
    msgpack_packet *mp;
    while (!msgpack_packets.empty()) {
        mp = msgpack_packets.top();
        Local<Value> v8obj = mp->v8obj;
        msgpack_object *mo = mp->mo;
        msgpack_zone *mz = mp->mz;
        size_t depth = mp->depth + 1;
        if (512 < depth) {
            throw MsgpackException("Cowardly refusing to pack object with circular reference");
        }
        msgpack_packets.pop(); 
        if (v8obj->IsUndefined() || v8obj->IsNull()) {
            mo->type = MSGPACK_OBJECT_NIL;
        } else if (v8obj->IsBoolean()) {
            mo->type = MSGPACK_OBJECT_BOOLEAN;
            mo->via.boolean = Nan::To<bool>(v8obj).FromJust();
        } else if (v8obj->IsNumber()) {
            double d = Nan::To<double>(v8obj).FromJust();
            if (trunc(d) != d) {
                mo->type = MSGPACK_OBJECT_FLOAT;
                mo->via.f64 = d;
            } else if (d > 0) {
                mo->type = MSGPACK_OBJECT_POSITIVE_INTEGER;
                mo->via.u64 = static_cast<uint64_t>(d);
            } else {
                mo->type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
                mo->via.i64 = static_cast<int64_t>(d);
            }
        } else if (v8obj->IsString()) {
            mo->type = MSGPACK_OBJECT_STR;
            mo->via.str.size = static_cast<uint32_t>(Nan::DecodeBytes(v8obj, Nan::Encoding::UTF8));
            mo->via.str.ptr = (char*) msgpack_zone_malloc(mz, mo->via.str.size);

            Nan::DecodeWrite((char*)mo->via.str.ptr, mo->via.str.size, v8obj, Nan::Encoding::UTF8);
        } else if (v8obj->IsDate()) {
            mo->type = MSGPACK_OBJECT_STR;
            Local<Date> date = Local<Date>::Cast(v8obj);
            Local<Function> func = Local<Function>::Cast(Nan::Get(date, Nan::New<String>("toISOString").ToLocalChecked()).ToLocalChecked());
            Local<Value> argv[1] = {};
            Local<Value> result = Nan::Call(func, date, 0, argv).ToLocalChecked();
            mo->via.str.size = static_cast<uint32_t>(Nan::DecodeBytes(result, Nan::Encoding::UTF8));
            mo->via.str.ptr = (char*) msgpack_zone_malloc(mz, mo->via.str.size);

            Nan::DecodeWrite((char*)mo->via.str.ptr, mo->via.str.size, result, Nan::Encoding::UTF8);
        } else if (v8obj->IsArray()) {
            Local<Object> o = Nan::To<v8::Object>(v8obj).ToLocalChecked();
            Local<Array> a = Local<Array>::Cast(o);

            mo->type = MSGPACK_OBJECT_ARRAY;
            mo->via.array.size = a->Length();
            mo->via.array.ptr = (msgpack_object*) msgpack_zone_malloc(
                mz,
                sizeof(msgpack_object) * mo->via.array.size
            );

            for (uint32_t i = 0; i < a->Length(); i++) {
                Local<Value> v = Nan::Get(a, i).ToLocalChecked();
                msgpack_packet *newmp = new msgpack_packet();
                newmp->depth = depth;
                newmp->mz = mz;
                newmp->mo = &mo->via.array.ptr[i];
                newmp->v8obj = v;
                msgpack_packets.push(newmp);
            }
        } else if (Buffer::HasInstance(v8obj)) {
            Local<Object> buf = Nan::To<Object>(v8obj).ToLocalChecked();

            mo->type = MSGPACK_OBJECT_BIN;
            mo->via.bin.size = static_cast<uint32_t>(Buffer::Length(buf));
            mo->via.bin.ptr = Buffer::Data(buf);
        } else {
            Local<Object> o = Nan::To<Object>(v8obj).ToLocalChecked();
            Local<String> toJSON = Nan::New<String>("toJSON").ToLocalChecked();
            // for o.toJSON()
            if (Nan::Has(o, toJSON).FromJust() && Nan::Get(o, toJSON).ToLocalChecked()->IsFunction()) {
                Local<Function> fn = Local<Function>::Cast(Nan::Get(o, toJSON).ToLocalChecked());
                msgpack_packet *newmp = new msgpack_packet();
                newmp->depth = depth;
                newmp->mz = mz;
                newmp->mo = mo;
                newmp->v8obj = Nan::Call(fn, o, 0, NULL).ToLocalChecked();
                msgpack_packets.push(newmp);
                continue;
            }

            Local<Array> a = Nan::GetPropertyNames(o).ToLocalChecked();

            mo->type = MSGPACK_OBJECT_MAP;
            mo->via.map.size = a->Length();
            mo->via.map.ptr = (msgpack_object_kv*) msgpack_zone_malloc(
                mz,
                sizeof(msgpack_object_kv) * mo->via.map.size
            );

            for (uint32_t i = 0; i < a->Length(); i++) {
                Local<Value> k = Nan::Get(a, i).ToLocalChecked();
                msgpack_packet *newmp1 = new msgpack_packet();
                newmp1->depth = depth;
                newmp1->mz = mz;
                newmp1->mo = &mo->via.map.ptr[i].key;
                newmp1->v8obj = k;
                msgpack_packets.push(newmp1);
                msgpack_packet *newmp2 = new msgpack_packet();
                newmp2->depth = depth;
                newmp2->mz = mz;
                newmp2->mo = &mo->via.map.ptr[i].val;
                newmp2->v8obj = Nan::Get(o, k).ToLocalChecked();
                msgpack_packets.push(newmp2);
            }
        }
    }
}


static Local<Value>
get_local_value(msgpack_packet_s *mpo, msgpack_object *mo) {
    mpo->mo = mo;

    switch (mo->type) {
    case MSGPACK_OBJECT_NIL:
        mpo->v8val = Nan::Null();
        return mpo->v8val;

    case MSGPACK_OBJECT_BOOLEAN:
        mpo->v8val = (mo->via.boolean) ?
            Nan::True() :
            Nan::False();
        return mpo->v8val;

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        // As per Issue #42, we need to use the base Number
        // class as opposed to the subclass Integer, since
        // only the former takes 64-bit inputs. Using the
        // Integer subclass will truncate 64-bit values.
        mpo->v8val = Nan::New<Number>(static_cast<double>(mo->via.u64));
        return mpo->v8val;

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        // See comment for MSGPACK_OBJECT_POSITIVE_INTEGER
        mpo->v8val =  Nan::New<Number>(static_cast<double>(mo->via.i64));
        return mpo->v8val;

    case MSGPACK_OBJECT_FLOAT:
        mpo->v8val =  Nan::New<Number>(mo->via.f64);
        return mpo->v8val;

    case MSGPACK_OBJECT_STR:
        mpo->v8val =  Nan::New<String>(mo->via.str.ptr, mo->via.str.size).ToLocalChecked();
        return mpo->v8val;

    case MSGPACK_OBJECT_BIN:
        mpo->v8val = Nan::CopyBuffer(mo->via.str.ptr, mo->via.bin.size).ToLocalChecked();
        return mpo->v8val;

    case MSGPACK_OBJECT_ARRAY:
        mpo->v8arr  = Nan::New<Array>(mo->via.array.size);
        return mpo->v8arr;

    case MSGPACK_OBJECT_MAP:
        mpo->v8obj =  Nan::New<Object>();
        return mpo->v8obj;

    default:
        throw MsgpackException("Encountered unknown MesssagePack object type");
    }
}

// Convert a MessagePack object to a V8 object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
static Local<Value>
msgpack_to_v8(msgpack_object *mo) {
    queue<msgpack_packet_s *> v8objs;
    msgpack_packet_s *mpo = new msgpack_packet_s();
    get_local_value(mpo, mo);
    v8objs.push(mpo);
    msgpack_packet_s *temp;
    while (!v8objs.empty())
    {
        temp = v8objs.front();
        msgpack_object *mo = temp->mo;
        v8objs.pop();
        if (mo->type == MSGPACK_OBJECT_ARRAY) {
            for (uint32_t i = 0; i < mo->via.array.size; i++) {
                msgpack_packet_s *mpo = new msgpack_packet_s();
                Nan::Set(temp->v8arr, i, get_local_value(mpo, &mo->via.array.ptr[i]));
                if (mo->via.array.ptr[i].type == MSGPACK_OBJECT_ARRAY || 
                    mo->via.array.ptr[i].type == MSGPACK_OBJECT_MAP){
                    v8objs.push(mpo);
                }
            }
        } else if (mo->type == MSGPACK_OBJECT_MAP) {
            for (uint32_t i = 0; i < mo->via.map.size; i++) {
                msgpack_packet_s *mpok = new msgpack_packet_s();
                msgpack_packet_s *mpov = new msgpack_packet_s();
                Nan::Set(
                    temp->v8obj,
                    get_local_value(mpok, &mo->via.map.ptr[i].key),
                    get_local_value(mpov, &mo->via.map.ptr[i].val)
                );
                if (mo->via.map.ptr[i].key.type == MSGPACK_OBJECT_ARRAY || 
                    mo->via.map.ptr[i].key.type == MSGPACK_OBJECT_MAP){
                    v8objs.push(mpok);
                }
                if (mo->via.map.ptr[i].val.type == MSGPACK_OBJECT_ARRAY || 
                    mo->via.map.ptr[i].val.type == MSGPACK_OBJECT_MAP){
                    v8objs.push(mpov);
                }
            }
        } else {
            msgpack_packet_s *mpo = new msgpack_packet_s();
            temp->v8val = get_local_value(mpo, mo);
        }
    }
    if (mo->type == MSGPACK_OBJECT_ARRAY) {
        return mpo->v8arr;
    } else if (mo->type == MSGPACK_OBJECT_MAP) {
        return mpo->v8obj;
    }
    return mpo->v8val;
}

// var buf = msgpack.pack(obj[, obj ...]);
//
// Returns a Buffer object representing the serialized state of the provided
// JavaScript object. If more arguments are provided, their serialized state
// will be accumulated to the end of the previous value(s).
//
// Any number of objects can be provided as arguments, and all will be
// serialized to the same bytestream, back-to-back.
static NAN_METHOD(pack) {
    Nan::HandleScope scope;

    msgpack_packer pk;
    MsgpackZone mz;
    msgpack_sbuffer *sb;

    if (!sbuffers.empty()) {
        sb = sbuffers.top();
        sbuffers.pop();
    } else {
        sb = msgpack_sbuffer_new();
    }

    msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);

    for (int i = 0; i < info.Length(); i++) {
        msgpack_object mo;

        try {
            v8_to_msgpack(info[i], &mo, &mz._mz, 0);
        } catch (MsgpackException e) {
            return Nan::ThrowError(e.getThrownException());
        }

        if (msgpack_pack_object(&pk, mo)) {
            return Nan::ThrowError("Error serializaing object");
        }
    }

    Local<Object> slowBuffer = Nan::NewBuffer(
        sb->data, sb->size, _free_sbuf, (void *)sb
    ).ToLocalChecked();

    return info.GetReturnValue().Set(slowBuffer);
}

// var o = msgpack.unpack(buf);
//
// Return the JavaScript object resulting from unpacking the contents of the
// specified buffer. If the buffer does not contain a complete object, the
// undefined value is returned.
static NAN_METHOD(unpack) {
    Nan::HandleScope scope;

    if (info.Length() < 0 || !Buffer::HasInstance(info[0])) {
        return Nan::ThrowTypeError("First argument must be a Buffer");
    }

    Local<Object> buf = Nan::To<Object>(info[0]).ToLocalChecked();

    MsgpackZone mz;
    msgpack_object mo;
    size_t off = 0;

    switch (msgpack_unpack(Buffer::Data(buf), Buffer::Length(buf), &off, &mz._mz, &mo)) {
    case MSGPACK_UNPACK_EXTRA_BYTES:
    case MSGPACK_UNPACK_SUCCESS:
        try {
            Nan::Set(
                Nan::GetFunction(Nan::New<FunctionTemplate>(msgpack_unpack_template)).ToLocalChecked(),
                Nan::New<String>("bytes_remaining").ToLocalChecked(),
                Nan::New<Integer>(static_cast<int32_t>(Buffer::Length(buf) - off))
            );
            return info.GetReturnValue().Set(msgpack_to_v8(&mo));
        } catch (MsgpackException e) {
            return Nan::ThrowError(e.getThrownException());
        }

    case MSGPACK_UNPACK_CONTINUE:
        return;

    default:
        return Nan::ThrowError("Error de-serializing object");
    }
}

NAN_MODULE_INIT(init) {
    Nan::Set(target, Nan::New<String>("pack").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(pack)).ToLocalChecked());

    // Go through this mess rather than call NODE_SET_METHOD so that we can set
    // a field on the function for 'bytes_remaining'.
    msgpack_unpack_template.Reset(Nan::New<FunctionTemplate>(unpack));

    Nan::Set(
        target,
        Nan::New<String>("unpack").ToLocalChecked(),
        Nan::GetFunction(Nan::New<FunctionTemplate>(msgpack_unpack_template)).ToLocalChecked()
    );
}

NODE_MODULE(msgpackBinding, init);
// vim:ts=4 sw=4 et
