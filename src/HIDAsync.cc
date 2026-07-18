// -*- C++ -*-

// Copyright Hans Huebner and contributors. All rights reserved.
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include <sstream>
#include <vector>
#include <queue>

#include "devices.h"
#include "util.h"
#include "HIDAsync.h"
#include "read.h"

#if defined(__APPLE__)
#include "../hidapi/mac/hidapi_darwin.h"
#endif

HIDAsync::HIDAsync(const Napi::CallbackInfo &info)
    : Napi::ObjectWrap<HIDAsync>(info)
{
  Napi::Env env = info.Env();

  if (info.Length() != 2 || !info[0].IsExternal() || !info[1].IsExternal())
  {
    Napi::TypeError::New(env, "HIDAsync constructor is not supported").ThrowAsJavaScriptException();
    return;
  }

  auto appCtx = ApplicationContext::get();
  if (!appCtx)
  {
    Napi::TypeError::New(env, "hidapi not initialized").ThrowAsJavaScriptException();
    return;
  }

  auto ptr = info[0].As<Napi::External<hid_device>>().Data();
  auto contextState = info[1].As<Napi::External<ContextState>>().Data();
  _hidHandle = std::make_shared<DeviceContext>(appCtx, ptr);
  read_state = nullptr;

  // Register with the env context, so the device job queue gets drained at env teardown
  contextState->RegisterDevice(_hidHandle);
}

class CloseWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  CloseWorker(
      Napi::Env &env, std::shared_ptr<DeviceContext> hid, std::shared_ptr<ReadThreadState> read_state)
      : PromiseAsyncWorker(env, hid),
        read_state(std::move(read_state)) {}

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (read_state)
    {
      read_state->abort = true;

      // Wait for the thread to terminate
      read_state->wait();

      read_state = nullptr;
    }

    if (context->hid)
    {
      hid_close(context->hid);
      context->hid = nullptr;
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    return env.Undefined();
  }

private:
  std::shared_ptr<ReadThreadState> read_state;
};

void HIDAsync::closeHandle()
{
  if (read_state)
  {
    read_state->abort = true;
    read_state = nullptr;
  }

  // hid_close is called by the destructor
  _hidHandle = nullptr;
}

class OpenByPathWorker : public PromiseAsyncWorker<ContextState *>
{
public:
  OpenByPathWorker(const Napi::Env &env, ContextState *context, std::string path, bool nonExclusive)
      : PromiseAsyncWorker(env, context),
        path(path),
        nonExclusive(nonExclusive) {}

  ~OpenByPathWorker()
  {
    if (dev)
    {
      // dev wasn't claimed
      hid_close(dev);
      dev = nullptr;
    }
  }

  // This code will be executed on the worker thread
  void Execute() override
  {
    std::unique_lock<std::mutex> lock(context->appCtx->enumerateLock);
#if defined(__APPLE__)
    // The exclusivity flag is process-global, so it must be applied while holding
    // the lock that serializes all open calls
    hid_darwin_set_open_exclusive(nonExclusive ? 0 : 1);
#else
    (void)nonExclusive;
#endif
    dev = hid_open_path(path.c_str());
    if (!dev)
    {
      SetError(appendLastHidError(nullptr, "cannot open device with path " + path));
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    auto ptr = Napi::External<hid_device>::New(env, dev);
    dev = nullptr; // ownership is passed to the HIDAsync instance
    return context->asyncCtor.New({ptr, Napi::External<ContextState>::New(env, context)});
  }

private:
  std::string path;
  bool nonExclusive;
  hid_device *dev = nullptr;
};

class OpenByUsbIdsWorker : public PromiseAsyncWorker<ContextState *>
{
public:
  OpenByUsbIdsWorker(const Napi::Env &env, ContextState *context, int vendorId, int productId, std::u16string serial, bool nonExclusive)
      : PromiseAsyncWorker(env, context),
        vendorId(vendorId),
        productId(productId),
        serial(serial),
        nonExclusive(nonExclusive) {}

  ~OpenByUsbIdsWorker()
  {
    if (dev)
    {
      // dev wasn't claimed
      hid_close(dev);
      dev = nullptr;
    }
  }

  // This code will be executed on the worker thread
  void Execute() override
  {
    std::unique_lock<std::mutex> lock(context->appCtx->enumerateLock);
#if defined(__APPLE__)
    hid_darwin_set_open_exclusive(nonExclusive ? 0 : 1);
#else
    (void)nonExclusive;
#endif

    std::wstring wserialstr;
    const wchar_t *wserialptr = nullptr;
    if (!serial.empty())
    {
      wserialstr = u16_to_wide(serial);
      wserialptr = wserialstr.c_str();
    }

    dev = hid_open(vendorId, productId, wserialptr);
    if (!dev)
    {
      std::ostringstream os;
      os << "cannot open device with vendor id 0x" << std::hex << vendorId << " and product id 0x" << productId;
      SetError(appendLastHidError(nullptr, os.str()));
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    auto ptr = Napi::External<hid_device>::New(env, dev);
    dev = nullptr; // ownership is passed to the HIDAsync instance
    return context->asyncCtor.New({ptr, Napi::External<ContextState>::New(env, context)});
  }

private:
  int vendorId;
  int productId;
  std::u16string serial;
  bool nonExclusive;
  hid_device *dev = nullptr;
};

Napi::Value HIDAsync::Create(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  auto argsLength = info.Length();

  if (argsLength < 1)
  {
    Napi::TypeError::New(env, "HIDAsync::Create requires at least one arguments").ThrowAsJavaScriptException();
    return env.Null();
  }

  void *data = info.Data();
  if (!data)
  {
    Napi::TypeError::New(env, "HIDAsync::Create missing constructor data").ThrowAsJavaScriptException();
    return env.Null();
  }
  ContextState *context = (ContextState *)data;

  bool isNonExclusiveBool = false;
  if (argsLength > 1)
  {
    // We check if we have an optional param
    if (info[argsLength - 1].IsObject())
    {
      argsLength -= 1;

#if defined(__APPLE__)
      Napi::Object options = info[argsLength].As<Napi::Object>();
      Napi::Value isNonExclusiveMode = options.Get("nonExclusive");
      if (!isNonExclusiveMode.IsBoolean())
      {
        Napi::TypeError::New(env, "nonExclusive flag should be a boolean").ThrowAsJavaScriptException();
        return env.Null();
      }
      isNonExclusiveBool = isNonExclusiveMode.As<Napi::Boolean>().Value();
#endif
    }
  }

  if (argsLength == 1)
  {
    // open by path
    if (!info[0].IsString())
    {
      Napi::TypeError::New(env, "Device path must be a string").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();

    return (new OpenByPathWorker(env, context, path, isNonExclusiveBool))->QueueAndRun();
  }
  else
  {
    if (!info[0].IsNumber() || !info[1].IsNumber())
    {
      Napi::TypeError::New(env, "VendorId and ProductId must be integers").ThrowAsJavaScriptException();
      return env.Null();
    }

    std::u16string serial;
    if (argsLength > 2)
    {
      if (!info[2].IsString())
      {
        Napi::TypeError::New(env, "Serial must be a string").ThrowAsJavaScriptException();
        return env.Null();
      }

      serial = info[2].As<Napi::String>().Utf16Value();
    }

    int32_t vendorId = info[0].As<Napi::Number>().Int32Value();
    int32_t productId = info[1].As<Napi::Number>().Int32Value();

    return (new OpenByUsbIdsWorker(env, context, vendorId, productId, serial, isNonExclusiveBool))->QueueAndRun();
  }
}

Napi::Value HIDAsync::readStart(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() < 1 || !info[0].IsFunction())
  {
    Napi::TypeError::New(env, "readStart requires a callback function").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (read_state && read_state->is_running())
  {
    Napi::TypeError::New(env, "read is already running").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto callback = info[0].As<Napi::Function>();
  read_state = start_read_helper(env, _hidHandle, callback);

  return env.Null();
}

class ReadStopWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  ReadStopWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      std::shared_ptr<ReadThreadState> read_state)
      : PromiseAsyncWorker(env, hid),
        read_state(std::move(read_state))
  {
  }
  ~ReadStopWorker()
  {
  }

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    read_state->abort = true;

    // Wait for the thread to terminate
    read_state->wait();

    read_state = nullptr;
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    return env.Undefined();
  }

private:
  std::shared_ptr<ReadThreadState> read_state;
};

Napi::Value HIDAsync::readStop(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || !read_state || !read_state->is_running())
  {
    // Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  auto result = (new ReadStopWorker(env, _hidHandle, std::move(read_state)))->QueueAndRun();

  // Ownership of read_state is transferred to ReadStopWorker
  read_state = nullptr;

  return result;
};

class ReadOnceWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  ReadOnceWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      int timeout)
      : PromiseAsyncWorker(env, hid),
        _timeout(timeout)
  {
  }
  ~ReadOnceWorker()
  {
    if (buffer)
    {
      delete[] buffer;
    }
  }

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      buffer = new unsigned char[READ_BUFF_MAXSIZE];
      // This is wordy, but necessary to get the correct non-blocking behaviour
      if (_timeout == -1)
      {
        returnedLength = hid_read(context->hid, buffer, READ_BUFF_MAXSIZE);
      }
      else
      {
        returnedLength = hid_read_timeout(context->hid, buffer, READ_BUFF_MAXSIZE, _timeout);
      }

      if (returnedLength < 0)
      {
        SetError(appendLastHidReadError(context->hid, "could not read data from device"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    auto result = Napi::Buffer<unsigned char>::Copy(env, buffer, returnedLength);

    return result;
  }

private:
  int returnedLength = 0;
  unsigned char *buffer = nullptr;
  int _timeout;
};

Napi::Value HIDAsync::read(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (read_state != nullptr && read_state->is_running())
  {
    Napi::TypeError::New(env, "Cannot use read while async read is running").ThrowAsJavaScriptException();
    return env.Null();
  }

  int timeout = -1;
  if (info.Length() != 0)
  {
    if (info[0].IsNumber())
    {
      timeout = info[0].As<Napi::Number>().Uint32Value();
    }
    else
    {
      Napi::TypeError::New(env, "time out parameter must be a number").ThrowAsJavaScriptException();
      return env.Null();
    }
  }

  return (new ReadOnceWorker(env, _hidHandle, timeout))->QueueAndRun();
}

class GetFeatureReportWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  GetFeatureReportWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      uint8_t reportId,
      int bufSize)
      : PromiseAsyncWorker(env, hid),
        bufferLength(bufSize)
  {
    buffer = new unsigned char[bufSize];
    buffer[0] = reportId;
  }
  ~GetFeatureReportWorker()
  {
    if (buffer)
    {
      delete[] buffer;
    }
  }

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      bufferLength = hid_get_feature_report(context->hid, buffer, bufferLength);
      if (bufferLength < 0)
      {
        SetError(appendLastHidError(context->hid, "could not get feature report from device"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    auto result = Napi::Buffer<unsigned char>::Copy(env, buffer, bufferLength);

    return result;
  }

private:
  unsigned char *buffer;
  int bufferLength;
};

Napi::Value HIDAsync::getFeatureReport(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() != 2 || !info[0].IsNumber() || !info[1].IsNumber())
  {
    Napi::TypeError::New(env, "need report ID and length parameters in getFeatureReport").ThrowAsJavaScriptException();
    return env.Null();
  }

  const uint8_t reportId = info[0].As<Napi::Number>().Uint32Value();
  const int bufSize = info[1].As<Napi::Number>().Uint32Value();
  if (bufSize <= 0)
  {
    Napi::TypeError::New(env, "Length parameter cannot be zero in getFeatureReport").ThrowAsJavaScriptException();
    return env.Null();
  }

  return (new GetFeatureReportWorker(env, _hidHandle, reportId, bufSize))->QueueAndRun();
}

class SendFeatureReportWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  SendFeatureReportWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      std::vector<unsigned char> srcBuffer)
      : PromiseAsyncWorker(env, hid),
        srcBuffer(srcBuffer) {}

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      written = hid_send_feature_report(context->hid, srcBuffer.data(), srcBuffer.size());
      if (written < 0)
      {
        SetError(appendLastHidError(context->hid, "could not send feature report to device"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    return Napi::Number::New(env, written);
  }

private:
  int written = 0;
  std::vector<unsigned char> srcBuffer;
};

Napi::Value HIDAsync::sendFeatureReport(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() != 1)
  {
    Napi::TypeError::New(env, "need report (including id in first byte) only in sendFeatureReportAsync").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::vector<unsigned char> message;
  std::string copyError = copyArrayOrBufferIntoVector(info[0], message);
  if (copyError != "")
  {
    Napi::TypeError::New(env, copyError).ThrowAsJavaScriptException();
    return env.Null();
  }

  return (new SendFeatureReportWorker(env, _hidHandle, message))->QueueAndRun();
}

Napi::Value HIDAsync::close(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device is already closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  // TODO - option to flush or purge queued operations

  // Mark it as closed, to stop new jobs being pushed to the queue
  _hidHandle->is_closed = true;

  auto result = (new CloseWorker(env, std::move(_hidHandle), std::move(read_state)))->QueueAndRun();

  // Ownership is transferred to CloseWorker
  _hidHandle = nullptr;
  read_state = nullptr;

  return result;
}

class SetNonBlockingWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  SetNonBlockingWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      int mode)
      : PromiseAsyncWorker(env, hid),
        mode(mode) {}

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      int res = hid_set_nonblocking(context->hid, mode);
      if (res < 0)
      {
        SetError(appendLastHidError(context->hid, "Error setting non-blocking mode"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    return env.Undefined();
  }

private:
  int mode;
};

Napi::Value HIDAsync::setNonBlocking(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() != 1)
  {
    Napi::TypeError::New(env, "Expecting a 1 to enable, 0 to disable as the first argument.").ThrowAsJavaScriptException();
    return env.Null();
  }

  int blockStatus = info[0].As<Napi::Number>().Int32Value();

  return (new SetNonBlockingWorker(env, _hidHandle, blockStatus))->QueueAndRun();
}

class WriteWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  WriteWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid,
      std::vector<unsigned char> srcBuffer)
      : PromiseAsyncWorker(env, hid),
        srcBuffer(srcBuffer) {}

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      written = hid_write(context->hid, srcBuffer.data(), srcBuffer.size());
      if (written < 0)
      {
        SetError(appendLastHidError(context->hid, "Cannot write to hid device"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    return Napi::Number::New(env, written);
  }

private:
  int written = 0;
  std::vector<unsigned char> srcBuffer;
};

Napi::Value HIDAsync::write(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  if (info.Length() != 1)
  {
    Napi::TypeError::New(env, "HID write requires one argument").ThrowAsJavaScriptException();
    return env.Null();
  }

  std::vector<unsigned char> message;
  std::string copyError = copyArrayOrBufferIntoVector(info[0], message);
  if (copyError != "")
  {
    Napi::TypeError::New(env, copyError).ThrowAsJavaScriptException();
    return env.Null();
  }

  return (new WriteWorker(env, _hidHandle, std::move(message)))->QueueAndRun();
}

class GetDeviceInfoWorker : public PromiseAsyncWorker<std::shared_ptr<DeviceContext>>
{
public:
  GetDeviceInfoWorker(
      Napi::Env &env,
      std::shared_ptr<DeviceContext> hid)
      : PromiseAsyncWorker(env, hid) {}

  // This code will be executed on the worker thread. Note: Napi types cannot be used
  void Execute() override
  {
    if (context->hid)
    {
      dev = hid_get_device_info(context->hid);
      if (!dev)
      {
        SetError(appendLastHidError(context->hid, "Unable to get device info"));
      }
    }
    else
    {
      SetError("device has been closed");
    }
  }

  Napi::Value GetPromiseResult(const Napi::Env &env) override
  {
    // if the hid device has somehow been deleted, the hid_device_info is no longer valid
    if (context->hid)
    {
      return generateDeviceInfo(env, dev);
    }
    else
    {
      return env.Null();
    }
  }

private:
  // this is owned by context->hid
  hid_device_info *dev = nullptr;
};
Napi::Value HIDAsync::getDeviceInfo(const Napi::CallbackInfo &info)
{
  Napi::Env env = info.Env();

  if (!_hidHandle || _hidHandle->is_closed)
  {
    Napi::TypeError::New(env, "device has been closed").ThrowAsJavaScriptException();
    return env.Null();
  }

  return (new GetDeviceInfoWorker(env, _hidHandle))->QueueAndRun();
}

Napi::Function HIDAsync::Initialize(Napi::Env &env)
{
  Napi::Function ctor = DefineClass(env, "HIDAsync", {
                                                         InstanceMethod("close", &HIDAsync::close),
                                                         InstanceMethod("readStart", &HIDAsync::readStart),
                                                         InstanceMethod("readStop", &HIDAsync::readStop),
                                                         InstanceMethod("write", &HIDAsync::write, napi_enumerable),
                                                         InstanceMethod("getFeatureReport", &HIDAsync::getFeatureReport, napi_enumerable),
                                                         InstanceMethod("sendFeatureReport", &HIDAsync::sendFeatureReport, napi_enumerable),
                                                         InstanceMethod("setNonBlocking", &HIDAsync::setNonBlocking, napi_enumerable),
                                                         InstanceMethod("read", &HIDAsync::read, napi_enumerable),
                                                         InstanceMethod("getDeviceInfo", &HIDAsync::getDeviceInfo, napi_enumerable),
                                                     });

  return ctor;
}
