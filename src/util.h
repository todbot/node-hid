#ifndef NODEHID_UTIL_H__
#define NODEHID_UTIL_H__

#define NAPI_VERSION 4
#include <napi.h>

#include <queue>
#include <vector>
#include <memory>

#include <hidapi.h>

#define READ_BUFF_MAXSIZE 2048

std::wstring u16_to_wide(const std::u16string &in);
std::u16string wide_to_u16(const std::wstring &in);

/**
 * Build an error message for a failed hidapi call, appending the reason
 * reported by hid_error (if any).
 * Pass nullptr as the device to read the global error (for hid_init/hid_open failures).
 * Note: not valid for hid_read/hid_read_timeout failures, use appendLastHidReadError
 */
std::string appendLastHidError(hid_device *hid, std::string message);

/**
 * Build an error message for a failed hid_read/hid_read_timeout call, appending
 * the reason reported by hid_read_error (if any). Reads record their errors in a
 * separate per-handle slot (they run concurrently with other operations), so
 * hid_error would report an unrelated, stale reason for them
 */
std::string appendLastHidReadError(hid_device *hid, std::string message);

/**
 * Convert a js value (either a buffer ot array of numbers) into a vector of bytes.
 * Returns a non-empty string upon failure
 */
std::string copyArrayOrBufferIntoVector(const Napi::Value &val, std::vector<unsigned char> &message);

/**
 * Application-wide shared state.
 * This is referenced by the main thread and every worker_thread where node-hid has been loaded and not yet unloaded.
 */
class ApplicationContext
{
public:
    ~ApplicationContext();

    static std::shared_ptr<ApplicationContext> get();

    // A lock for any enumerate/open operations, as they are not thread safe
    // In async land, these are also done in a single-threaded queue, this lock is used to link up with the sync side
    std::mutex enumerateLock;
};

class AsyncWorkerQueue
{
public:
    ~AsyncWorkerQueue();

    /**
     * Push a job onto the queue.
     * Note: This must only be run from the main thread
     */
    void QueueJob(const Napi::Env &, Napi::AsyncWorker *job);

    /**
     * The job has finished, start the next in the queue.
     * Note: This must only be run from the main thread
     */
    void JobFinished(const Napi::Env &);

    /**
     * Delete any jobs which are queued but have not been started.
     * The currently running job (if any) is left alone; it owns its own destruction.
     * Their promises are never settled - this is only for env teardown, where no JS
     * will run again to observe a settlement.
     * Note: This must only be run from the main thread
     */
    void DrainQueue();

private:
    bool isRunning = false;
    std::queue<Napi::AsyncWorker *> jobQueue;
    std::mutex jobQueueMutex;
};

class DeviceContext;

/**
 * Context-wide shared state.
 * One of these will be created for each Napi::Env (main thread and each worker_thread)
 */
class ContextState : public AsyncWorkerQueue
{
public:
    ContextState(std::shared_ptr<ApplicationContext> appCtx, Napi::FunctionReference asyncCtor) : AsyncWorkerQueue(), appCtx(appCtx), asyncCtor(std::move(asyncCtor)) {}

    // Keep the ApplicationContext alive for longer than this state
    std::shared_ptr<ApplicationContext> appCtx;

    // Constructor for the HIDAsync class
    Napi::FunctionReference asyncCtor;

    /**
     * Track a DeviceContext created for this env, so that its job queue can be
     * drained at env teardown.
     * Note: This must only be run from the main thread
     */
    void RegisterDevice(const std::shared_ptr<DeviceContext> &device);

    /**
     * Drain this queue and the queue of every still-live registered DeviceContext.
     * Deleting the queued workers releases their shared_ptr references, breaking the
     * queue<->worker cycle so the DeviceContext (and its hid handle) can be freed.
     * Called from the env cleanup hook; must only be run from the main thread
     */
    void DrainAllQueues();

private:
    std::mutex devicesMutex;
    std::vector<std::weak_ptr<DeviceContext>> devices;
};

class DeviceContext : public AsyncWorkerQueue
{
public:
    DeviceContext(std::shared_ptr<ApplicationContext> appCtx, hid_device *hidHandle) : AsyncWorkerQueue(), hid(hidHandle), appCtx(appCtx)
    {
    }

    ~DeviceContext();

    hid_device *hid;

    bool is_closed = false;

private:
    // Hold a reference to the ApplicationContext,
    std::shared_ptr<ApplicationContext> appCtx;
};

template <class T>
class PromiseAsyncWorker : public Napi::AsyncWorker
{
public:
    PromiseAsyncWorker(
        const Napi::Env &env, T context)
        : Napi::AsyncWorker(env),
          context(context),
          deferred(Napi::Promise::Deferred::New(env)),
          // Create an error now, to store the stack trace
          errorResult(Napi::Error::New(env, "Unknown error"))
    {
    }

    // This code will be executed on the worker thread. Note: Napi types cannot be used
    virtual void Execute() override = 0;

    virtual Napi::Value GetPromiseResult(const Napi::Env &env) = 0;

    void OnOK() override
    {
        Napi::Env env = Env();

        // Collect the result before finishing the job, in case the result relies on the hid object.
        // A throw from GetPromiseResult must not skip JobFinished, or the queue stalls forever
        Napi::Value result;
        try
        {
            result = GetPromiseResult(env);
        }
        catch (const Napi::Error &e)
        {
            context->JobFinished(env);
            deferred.Reject(e.Value());
            return;
        }
        catch (const std::exception &e)
        {
            context->JobFinished(env);
            deferred.Reject(Napi::Error::New(env, e.what()).Value());
            return;
        }

        context->JobFinished(env);

        deferred.Resolve(result);
    }
    void OnError(Napi::Error const &error) override
    {
        // Inject the the error message with the actual error
        errorResult.Value().Set("message", error.Message());

        context->JobFinished(Env());
        deferred.Reject(errorResult.Value());
    }

    void OnWorkComplete(Napi::Env env, napi_status status) override
    {
        try
        {
            Napi::AsyncWorker::OnWorkComplete(env, status);
        }
        catch (...)
        {
            // The env is terminating (eg worker_threads terminate()), so settling the
            // promise failed and rethrowing into JS also failed, skipping the internal
            // Destroy(). Without this catch the exception escapes into libuv and aborts
            // the whole process. Nothing can observe the promise now; just make sure
            // this worker is still freed
            Destroy();
        }
    }

    Napi::Promise QueueAndRun()
    {
        auto promise = deferred.Promise();

        context->QueueJob(Env(), this);

        return promise;
    }

protected:
    T context;

private:
    Napi::Promise::Deferred deferred;
    Napi::Error errorResult;
};

#endif // NODEHID_UTIL_H__