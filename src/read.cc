#include "read.h"

struct ReadCallbackContext;

#include <memory>

struct ReadCallbackProps
{
    // Adopted from the read loop; freed with the struct
    std::unique_ptr<unsigned char[]> buf;
    int len;
    // Set when len < 0: the reason reported by hid_read_error, captured on the
    // read thread while it is still fresh
    std::string error;
};

using Context = ReadCallbackContext;
using DataType = ReadCallbackProps;
void ReadCallback(Napi::Env env, Napi::Function callback, Context *context, DataType *data);
using TSFN = Napi::TypedThreadSafeFunction<Context, DataType, ReadCallback>;
using FinalizerDataType = void;

struct ReadCallbackContext
{
    std::shared_ptr<ReadThreadState> state;

    std::shared_ptr<DeviceContext> _hidHandle;
    std::thread read_thread;

    TSFN read_callback;
};

void ReadCallback(Napi::Env env, Napi::Function callback, Context *context, DataType *data)
{
    if (env != nullptr && callback != nullptr) //&& context != nullptr)
    {
        if (data == nullptr || data->len < 0)
        {
            auto error = Napi::String::New(env, data != nullptr ? data->error : "could not read from HID device");

            callback.Call({error, env.Null()});
        }
        else
        {
            // Copying this buffer is a bit excessive for nodejs, but necessary for electron
            // It should be cheap enough, as read data is usually in the scale of a handful of bytes
            auto buffer = Napi::Buffer<unsigned char>::Copy(env, data->buf.get(), data->len);

            callback.Call({env.Null(), buffer});
        }
    }

    delete data;
};

bool ReadThreadState::is_running()
{
    std::unique_lock<std::mutex> lk(lock);
    return running;
}
void ReadThreadState::wait()
{
    std::unique_lock<std::mutex> lk(lock);
    if (running)
    {
        wait_for_end.wait(lk, [this]
                          { return !running; });
    }
}

void ReadThreadState::release()
{
    std::unique_lock<std::mutex> lk(lock);
    running = false;
    wait_for_end.notify_all();
}

/**
 * Getting the thread safety of this correct has been challenging.
 * There is a problem that the read thread can take 50ms to exit once run_read becomes false, and we don't want to block the event loop waiting for it.
 * And a problem of that either the read_thread can decide to abort by itself.
 * This means that either read_thread or the class that spawned it could outlive the other.
 *
 * Doing this in a class is hard, as the tsfn needs to be the one to own and free the class, but the caller needs to be able to 'ask' for an abort.
 * This method is a simplified form, using a 'context' class whose ownership gets assigned to the tsfn, with the caller only getting the atomic<bool>
 * to both be able to know if the loop is running and to 'ask' it to stop
 *
 * While this does now return a struct to handle the shared state, the tsfn and thread are importantly not on this class.
 */
std::shared_ptr<ReadThreadState> start_read_helper(Napi::Env env, std::shared_ptr<DeviceContext> hidHandle, Napi::Function callback)
{
    auto state = std::make_shared<ReadThreadState>();

    auto context = new ReadCallbackContext;
    context->state = state;
    context->_hidHandle = std::move(hidHandle);

    // The tsfn must be created before the thread is spawned, as the thread uses it
    // immediately (a dead device makes the first hid_read_timeout return instantly).
    // The reverse ordering is safe: the finalizer (which joins read_thread) can only run
    // on this thread via the event loop, so not before this function has returned and
    // read_thread has been assigned. An early Release() by the thread merely queues it.
    context->read_callback = TSFN::New(
        env,
        callback,                                 // JavaScript function called asynchronously
        "HID:read",                               // Name
        0,                                        // Unlimited queue
        1,                                        // Only one thread will use this initially
        context,                                  // Context
        [](Napi::Env, void *, Context *context) { // Finalizer used to clean threads up
            if (context->read_thread.joinable())
            {
                // Ensure the thread has terminated
                context->read_thread.join();
            }

            // Free the context
            delete context;
        });

    context->read_thread = std::thread([context]()
                                       {
                              int mswait = 50;
                              int len = 0;
                              auto buf = std::unique_ptr<unsigned char[]>(new unsigned char[READ_BUFF_MAXSIZE]);
                              bool tsfn_usable = true;

                              while (!context->state->abort)
                              {
                                len = hid_read_timeout(context->_hidHandle->hid, buf.get(), READ_BUFF_MAXSIZE, mswait);
                                if (context->state->abort)
                                    break;

                                if (len < 0)
                                {
                                    // Emit an error and stop reading
                                    auto data = new ReadCallbackProps{
                                        nullptr, len,
                                        appendLastHidReadError(context->_hidHandle->hid, "could not read from HID device")};

                                    napi_status status = context->read_callback.BlockingCall(data);
                                    if (status != napi_ok)
                                    {
                                        // The call was not queued, so ReadCallback will never free data
                                        delete data;
                                        if (status == napi_closing)
                                            tsfn_usable = false;
                                    }
                                    break;
                                }
                                else if (len > 0)
                                {
                                    auto data = new ReadCallbackProps{std::move(buf), len, {}};
                                    buf = std::unique_ptr<unsigned char[]>(new unsigned char[READ_BUFF_MAXSIZE]);

                                    napi_status status = context->read_callback.BlockingCall(data);
                                    if (status != napi_ok)
                                    {
                                        // The call was not queued, so ReadCallback will never free data
                                        delete data;
                                        if (status == napi_closing)
                                            tsfn_usable = false;
                                        break;
                                    }
                                }
                              }

                              // Mark the state and used hidHandle as released
                              context->state->release();

                              // Cleanup the function. After napi_closing the tsfn must not be touched again.
                              if (tsfn_usable)
                                  context->read_callback.Release(); });

    return state;
}