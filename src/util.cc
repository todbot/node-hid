#include <sstream>

#include "util.h"

// Ensure hid_init/hid_exit is coordinated across all threads. Global data is bad for context-aware modules, but this is designed to be safe
std::mutex lockApplicationContext;
std::weak_ptr<ApplicationContext> weakApplicationContext; // This will let it be garbage collected when it goes out of scope in the last thread

ApplicationContext::~ApplicationContext()
{
    // Make sure we dont try to aquire it or run init at the same time
    std::unique_lock<std::mutex> lock(lockApplicationContext);

    if (hid_exit())
    {
        // thread is exiting, can't log?
    }
}

std::shared_ptr<ApplicationContext> ApplicationContext::get()
{
    // Make sure that we don't try to lock the pointer while it is being freed
    // and that two threads don't try to create it concurrently
    std::unique_lock<std::mutex> lock(lockApplicationContext);

    auto ref = weakApplicationContext.lock();
    if (!ref)
    {
        // Not initialised, so lets do that
        if (hid_init())
        {
            return nullptr;
        }

        ref = std::make_shared<ApplicationContext>();
        weakApplicationContext = ref;
    }
    return ref;
}

std::string utf8_encode(const std::wstring &source)
{
    std::string result;
    for (size_t i = 0; i < source.size(); ) {
        uint32_t cp;
        wchar_t wc = source[i++];
#ifdef _WIN32
        // wchar_t is UTF-16 on Windows; handle surrogate pairs
        if (wc >= 0xD800 && wc <= 0xDBFF && i < source.size()) {
            wchar_t wc2 = source[i];
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                cp = 0x10000u + ((static_cast<uint32_t>(wc - 0xD800) << 10) | (wc2 - 0xDC00));
                i++;
            } else {
                cp = static_cast<uint32_t>(wc);
            }
        } else {
            cp = static_cast<uint32_t>(wc);
        }
#else
        cp = static_cast<uint32_t>(wc);
#endif
        if (cp < 0x80u) {
            result += static_cast<char>(cp);
        } else if (cp < 0x800u) {
            result += static_cast<char>(0xC0u | (cp >> 6));
            result += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            result += static_cast<char>(0xE0u | (cp >> 12));
            result += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            result += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
            result += static_cast<char>(0xF0u | (cp >> 18));
            result += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            result += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            result += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
    }
    return result;
}

std::wstring utf8_decode(const std::string &source)
{
    std::wstring result;
    size_t i = 0;
    while (i < source.size()) {
        auto c = static_cast<unsigned char>(source[i]);
        uint32_t cp;
        if (c < 0x80u) {
            cp = c; i++;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < source.size()) {
            cp = (static_cast<uint32_t>(c & 0x1Fu) << 6)
               |  static_cast<uint32_t>(static_cast<unsigned char>(source[i+1]) & 0x3Fu);
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < source.size()) {
            cp = (static_cast<uint32_t>(c & 0x0Fu) << 12)
               | (static_cast<uint32_t>(static_cast<unsigned char>(source[i+1]) & 0x3Fu) << 6)
               |  static_cast<uint32_t>(static_cast<unsigned char>(source[i+2]) & 0x3Fu);
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < source.size()) {
            cp = (static_cast<uint32_t>(c & 0x07u) << 18)
               | (static_cast<uint32_t>(static_cast<unsigned char>(source[i+1]) & 0x3Fu) << 12)
               | (static_cast<uint32_t>(static_cast<unsigned char>(source[i+2]) & 0x3Fu) << 6)
               |  static_cast<uint32_t>(static_cast<unsigned char>(source[i+3]) & 0x3Fu);
            i += 4;
        } else {
            i++; continue; // invalid byte, skip
        }
#ifdef _WIN32
        // wchar_t is UTF-16 on Windows; emit surrogate pair if needed
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            result += static_cast<wchar_t>(0xD800u + (cp >> 10));
            result += static_cast<wchar_t>(0xDC00u + (cp & 0x3FFu));
        } else {
            result += static_cast<wchar_t>(cp);
        }
#else
        result += static_cast<wchar_t>(cp);
#endif
    }
    return result;
}

std::string copyArrayOrBufferIntoVector(const Napi::Value &val, std::vector<unsigned char> &message)
{
    if (val.IsBuffer())
    {
        Napi::Buffer<unsigned char> buffer = val.As<Napi::Buffer<unsigned char>>();
        uint32_t len = buffer.Length();
        unsigned char *data = buffer.Data();
        message.assign(data, data + len);

        return "";
    }
    else if (val.IsArray())
    {
        Napi::Array messageArray = val.As<Napi::Array>();
        message.reserve(messageArray.Length());

        for (unsigned i = 0; i < messageArray.Length(); i++)
        {
            Napi::Value v = messageArray.Get(i);
            if (!v.IsNumber())
            {
                return "unexpected array element in array to send, expecting only integers";
            }
            uint32_t b = v.As<Napi::Number>().Uint32Value();
            message.push_back((unsigned char)b);
        }

        return "";
    }
    else
    {
        return "unexpected data to send, expecting an array or buffer";
    }
}

DeviceContext::~DeviceContext()
{
    if (hid)
    {
        // We shouldn't ever get here, but lets make sure it was freed
        hid_close(hid);
        hid = nullptr;
    }
}

void AsyncWorkerQueue::QueueJob(const Napi::Env &, Napi::AsyncWorker *job)
{
    std::unique_lock<std::mutex> lock(jobQueueMutex);
    if (!isRunning)
    {
        isRunning = true;
        job->Queue();
    }
    else
    {
        jobQueue.push(job);
    }
}

void AsyncWorkerQueue::JobFinished(const Napi::Env &)
{
    std::unique_lock<std::mutex> lock(jobQueueMutex);

    if (jobQueue.size() == 0)
    {
        isRunning = false;
    }
    else
    {
        auto newJob = jobQueue.front();
        jobQueue.pop();
        newJob->Queue();
    }
}
