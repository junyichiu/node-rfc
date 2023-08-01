// SPDX-FileCopyrightText: 2014 SAP SE Srdjan Boskovic <srdjan.boskovic@sap.com>
//
// SPDX-License-Identifier: Apache-2.0
#include "Server.h"
#include <node_api.h>

namespace node_rfc {
extern Napi::Env __env;
extern char const* USAGE_URL;

uint_t Server::_id = 1;

Server* __server = NULL;

Napi::Object Server::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);

  Napi::Function func =
      DefineClass(env,
                  "Server",
                  {
                      InstanceAccessor("_id", &Server::IdGetter, nullptr),
                      InstanceAccessor("_alive", &Server::AliveGetter, nullptr),
                      InstanceAccessor("_server_conn_handle",
                                       &Server::ServerConnectionHandleGetter,
                                       nullptr),
                      InstanceAccessor("_client_conn_handle",
                                       &Server::ClientConnectionHandleGetter,
                                       nullptr),
                      InstanceMethod("start", &Server::Start),
                      InstanceMethod("stop", &Server::Stop),
                      InstanceMethod("addFunction", &Server::AddFunction),
                      InstanceMethod("removeFunction", &Server::RemoveFunction),
                      InstanceMethod("getFunctionDescription",
                                     &Server::GetFunctionDescription),
                  });

  Napi::FunctionReference* constructor = new Napi::FunctionReference();
  *constructor = Napi::Persistent(func);
  constructor->SuppressDestruct();

  exports.Set("Server", func);
  return exports;
}

Napi::Value Server::IdGetter(const Napi::CallbackInfo& info) {
  return Napi::Number::New(info.Env(), id);
}

Napi::Value Server::AliveGetter(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), server_conn_handle != NULL);
}

Napi::Value Server::ServerConnectionHandleGetter(
    const Napi::CallbackInfo& info) {
  return Napi::Number::New(
      info.Env(), (double)(unsigned long long)this->server_conn_handle);
}
Napi::Value Server::ClientConnectionHandleGetter(
    const Napi::CallbackInfo& info) {
  return Napi::Number::New(
      info.Env(), (double)(unsigned long long)this->client_conn_handle);
}

Server::Server(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<Server>(info) {
  node_rfc::__server = this;

  init();

  DEBUG("Server::Server ", id);

  if (!info[0].IsObject()) {
    Napi::TypeError::New(Env(), "Server constructor requires server parameters")
        .ThrowAsJavaScriptException();
    return;
  }

  serverParamsRef = Napi::Persistent(info[0].As<Napi::Object>());
  getConnectionParams(serverParamsRef.Value(), &server_params);

  if (!info[1].IsObject()) {
    Napi::TypeError::New(Env(), "Server constructor requires client parameters")
        .ThrowAsJavaScriptException();
    return;
  }

  clientParamsRef = Napi::Persistent(info[1].As<Napi::Object>());
  getConnectionParams(clientParamsRef.Value(), &client_params);

  if (!info[2].IsUndefined()) {
    if (!info[2].IsObject()) {
      Napi::TypeError::New(
          Env(), "Server constructor client options must be an object")
          .ThrowAsJavaScriptException();
      return;
    }
    clientOptionsRef = Napi::Persistent(info[2].As<Napi::Object>());
    checkClientOptions(clientOptionsRef.Value(), &client_options);
  }
};

RFC_RC SAP_API metadataLookup(SAP_UC const* func_name,
                              RFC_ATTRIBUTES rfc_attributes,
                              RFC_FUNCTION_DESC_HANDLE* func_desc_handle) {
  UNUSED(rfc_attributes);

  printf("Metadata lookup for: ");
  printfU(func_name);
  printf("\n");

  RFC_RC rc = RFC_NOT_FOUND;

  Server* server = node_rfc::__server;  // todo check if null

  ServerFunctionsMap::iterator it = server->serverFunctions.begin();
  while (it != server->serverFunctions.end()) {
    if (strcmpU(func_name, it->second->func_name) == 0) {
      *func_desc_handle = it->second->func_desc_handle;
      rc = RFC_OK;
      DEBUG("\nmetadataLookup found: ", (pointer_t)*func_desc_handle);
      break;
    }
    ++it;
  }

  return rc;
}

Napi::Value wrapUnitIdentifier(RFC_UNIT_IDENTIFIER* uIdentifier) {
  Napi::Object unitIdentifier = Napi::Object::New(node_rfc::__env);
  unitIdentifier.Set("queued", wrapString(&uIdentifier->unitType, 1));
  unitIdentifier.Set("id", wrapString(uIdentifier->unitID));
  return unitIdentifier;
}

Napi::Value getServerRequestContext(RFC_CONNECTION_HANDLE conn_handle,
                                    RFC_ERROR_INFO* serverErrorInfo) {
  Napi::Object requestContext = Napi::Object::New(node_rfc::__env);

  RFC_SERVER_CONTEXT context;

  RFC_RC rc = RfcGetServerContext(conn_handle, &context, serverErrorInfo);

  if (rc != RFC_OK || serverErrorInfo->code != RFC_OK) return requestContext;

  requestContext.Set("callType",
                     Napi::Number::New(node_rfc::__env, context.type));
  // [ "synchronous", "transactional", "queued", "background_unit" ];
  requestContext.Set(
      "isStateful",
      Napi::Boolean::New(node_rfc::__env, context.isStateful != 0));
  if (context.type != RFC_SYNCHRONOUS) {
    requestContext.Set("unitIdentifier",
                       wrapUnitIdentifier(context.unitIdentifier));
  }
  // if (context.type == RFC_BACKGROUND_UNIT) {
  //   requestContext.Set("unitAttributes",
  //       wrapUnitAttributes(context.unitAttributes);
  // }
  return requestContext;
}

RFC_RC SAP_API genericRequestHandler(RFC_CONNECTION_HANDLE conn_handle,
                                     RFC_FUNCTION_HANDLE func_handle,
                                     RFC_ERROR_INFO* errorInfo) {
  UNUSED(conn_handle);

  Server* server = node_rfc::__server;

  RFC_RC rc = RFC_NOT_FOUND;

  RFC_FUNCTION_DESC_HANDLE func_desc =
      RfcDescribeFunction(func_handle, errorInfo);
  if (errorInfo->code != RFC_OK) {
    return errorInfo->code;
  }
  RFC_ABAP_NAME func_name;
  RfcGetFunctionName(func_desc, func_name, errorInfo);
  if (errorInfo->code != RFC_OK) {
    return errorInfo->code;
  }

  printf("genericRequestHandler for: ");
  printfU(func_name);
  printf(" func_handle: %p\n", (void*)func_handle);

  ServerFunctionsMap::iterator it = server->serverFunctions.begin();
  while (it != server->serverFunctions.end()) {
    if (strcmpU(func_name, it->second->func_name) == 0) {
      printf("found func_desc %p\n", (void*)it->second->func_desc_handle);
      break;
    }
    ++it;
  }

  if (it == server->serverFunctions.end()) {
    printf("not found!\n");
    return rc;
  }

  ServerRequestBaton* requestBaton = new ServerRequestBaton();
  auto errorPath = node_rfc::RfmErrorPath();

  requestBaton->request_connection_handle = conn_handle;
  requestBaton->client_options = server->client_options;
  requestBaton->func_handle = func_handle;
  requestBaton->func_desc_handle = it->second->func_desc_handle;
  requestBaton->errorInfo = errorInfo;
  requestBaton->errorPath = errorPath;

  DEBUG("genericRequestHandler tsfnRequest.BlockingCall: start");

  it->second->tsfnRequest.BlockingCall(requestBaton);

  requestBaton->wait();

  DEBUG("genericRequestHandler tsfnRequest.BlockingCall: done");

  return RFC_OK;
}

class StartAsync : public Napi::AsyncWorker {
 public:
  StartAsync(Napi::Function& callback, Server* server)
      : Napi::AsyncWorker(callback), server(server) {}
  ~StartAsync() {}

  void Execute() {
    server->LockMutex();
    DEBUG("StartAsync locked");
    server->client_conn_handle =
        RfcOpenConnection(server->client_params.connectionParams,
                          server->client_params.paramSize,
                          &errorInfo);
    if (errorInfo.code != RFC_OK) {
      return;
    }
    DEBUG("Server:: client connection ok");

    server->server_conn_handle =
        RfcRegisterServer(server->server_params.connectionParams,
                          server->server_params.paramSize,
                          &errorInfo);
    if (errorInfo.code != RFC_OK) {
      return;
    }
    DEBUG("Server:: registered");

    RfcInstallGenericServerFunction(
        genericRequestHandler, metadataLookup, &errorInfo);
    if (errorInfo.code != RFC_OK) {
      return;
    }
    DEBUG("Server:: installed");

    server->serverHandle =
        RfcCreateServer(server->server_params.connectionParams, 1, &errorInfo);
    if (errorInfo.code != RFC_OK) {
      return;
    }
    DEBUG("Server:: created");

    RfcLaunchServer(server->serverHandle, &errorInfo);
    if (errorInfo.code != RFC_OK) {
      return;
    }
    DEBUG("Server:: launched ", (pointer_t)server->serverHandle)

    server->UnlockMutex();
    DEBUG("StartAsync unlocked");
  }

  void OnOK() {
    Napi::EscapableHandleScope scope(Env());
    if (errorInfo.code != RFC_OK) {
      // Callback().Call({Env().Undefined()});
      Callback().Call({rfcSdkError(&errorInfo)});
    } else {
      Callback().Call({});
    }
    Callback().Reset();
  }

 private:
  Server* server;
  RFC_ERROR_INFO errorInfo;
};

class GetFunctionDescAsync : public Napi::AsyncWorker {
 public:
  GetFunctionDescAsync(Napi::Function& callback, Server* server)
      : Napi::AsyncWorker(callback), server(server) {}
  ~GetFunctionDescAsync() {}

  void Execute() {
    server->LockMutex();
    server->server_conn_handle =
        RfcRegisterServer(server->server_params.connectionParams,
                          server->server_params.paramSize,
                          &errorInfo);
    server->UnlockMutex();
  }

  void OnOK() {
    if (server->server_conn_handle == NULL) {
      Callback().Call({rfcSdkError(&errorInfo)});
    } else {
      Callback().Call({});
    }
    Callback().Reset();
  }

 private:
  Server* server;
  RFC_ERROR_INFO errorInfo;
};

Napi::Value Server::Start(const Napi::CallbackInfo& info) {
  DEBUG("Server::Serve");

  std::ostringstream errmsg;

  if (!info[0].IsFunction()) {
    errmsg << "Server start() requires a callback function; see" << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::Function callback = info[0].As<Napi::Function>();

  (new StartAsync(callback, this))->Queue();

  return info.Env().Undefined();
};

Napi::Value Server::Stop(const Napi::CallbackInfo& info) {
  DEBUG("Server::Stop");

  std::ostringstream errmsg;

  if (!info[0].IsFunction()) {
    errmsg << "Server stop() requires a callback function; see" << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  // Napi::Function callback = info[0].As<Napi::Function>();

  //(new StopAsync(callback, this))->Queue();

  return info.Env().Undefined();
};

Napi::Value Server::AddFunction(const Napi::CallbackInfo& info) {
  Napi::EscapableHandleScope scope(info.Env());

  std::ostringstream errmsg;

  if (!info[0].IsString()) {
    errmsg << "Server addFunction() requires ABAP RFM name; see" << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::String functionName = info[0].As<Napi::String>();

  if (functionName.Utf8Value().length() == 0 ||
      functionName.Utf8Value().length() > 30) {
    errmsg << "Server addFunction() accepts max. 30 characters long ABAP RFM "
              "name; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  DEBUG("Server::AddFunction ", functionName.Utf8Value());

  if (!info[1].IsFunction()) {
    errmsg << "Server addFunction() requires a NodeJS handler function; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::Function jsFunction = info[1].As<Napi::Function>();

  if (!info[2].IsFunction()) {
    errmsg << "Server addFunction() requires a callback function; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::Function callback = info[2].As<Napi::Function>();

  // Install function
  RFC_ERROR_INFO errorInfo;

  SAP_UC* func_name = setString(functionName);

  RFC_FUNCTION_DESC_HANDLE func_desc_handle =
      RfcGetFunctionDesc(client_conn_handle, func_name, &errorInfo);

  if (errorInfo.code != RFC_OK) {
    delete[] func_name;
    callback.Call({rfcSdkError(&errorInfo)});
    return scope.Escape(info.Env().Undefined());
  }

  // Create thread-safe function to be called by genericRequestHandler
  ServerRequestTsfn tsfnRequest = ServerRequestTsfn::New(
      info.Env(),
      jsFunction,           // JavaScript function called asynchronously
      "ServerRequestTsfn",  // Resource name
      0,                    // Unlimited queue
      1,                    // Only one thread will use this initially
      nullptr               // No context needed
  );

  serverFunctions[functionName.Utf8Value()] =
      new ServerFunctionStruct(func_name, func_desc_handle, tsfnRequest);
  delete[] func_name;
  DEBUG("Server::AddFunction ",
        functionName.Utf8Value(),
        ": ",
        (pointer_t)func_desc_handle);

  callback.Call({});
  return scope.Escape(info.Env().Undefined());
};

Napi::Value Server::RemoveFunction(const Napi::CallbackInfo& info) {
  Napi::EscapableHandleScope scope(info.Env());

  std::ostringstream errmsg;

  if (!info[0].IsString()) {
    errmsg << "Server removeFunction() requires ABAP RFM name; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::String functionName = info[0].As<Napi::String>();

  if (functionName.Utf8Value().length() == 0 ||
      functionName.Utf8Value().length() > 30) {
    errmsg << "Server removeFunction() accepts max. 30 characters long ABAP "
              "RFM name; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  DEBUG("Server::RemoveFunction ", functionName.Utf8Value());

  if (!info[1].IsFunction()) {
    errmsg << "Server removeFunction() requires a callback function; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::Function callback = info[1].As<Napi::Function>();

  // Remove function

  SAP_UC* func_name = setString(functionName);

  ServerFunctionsMap::iterator it = serverFunctions.begin();
  while (it != serverFunctions.end()) {
    if (strcmpU(func_name, it->second->func_name) == 0) {
      break;
    }
    ++it;
  }
  delete[] func_name;

  if (it == serverFunctions.end()) {
    errmsg << "Server removeFunction() did not find function: "
           << functionName.Utf8Value();
    callback.Call({nodeRfcError(errmsg.str())});
    return scope.Escape(info.Env().Undefined());
  }

  DEBUG("Server::RemoveFunction removed ",
        functionName.Utf8Value(),
        ": ",
        (pointer_t)it->second->func_desc_handle);
  serverFunctions.erase(it);

  callback.Call({});
  return scope.Escape(info.Env().Undefined());
};

Napi::Value Server::GetFunctionDescription(const Napi::CallbackInfo& info) {
  DEBUG("Server::GetFunctionDescription");

  std::ostringstream errmsg;

  if (!info[0].IsString()) {
    errmsg << "Server getFunctionDescription() requires ABAP RFM name; see"
           << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  if (!info[0].IsFunction()) {
    errmsg
        << "Server getFunctionDescription() requires a callback function; see"
        << USAGE_URL;
    Napi::TypeError::New(info.Env(), errmsg.str()).ThrowAsJavaScriptException();
    return info.Env().Undefined();
  }

  Napi::Function callback = info[1].As<Napi::Function>();

  (new GetFunctionDescAsync(callback, this))->Queue();

  return info.Env().Undefined();
};

Server::~Server(void) {
  DEBUG("~ Server ", id);

  if (serverHandle != NULL) {
    RfcShutdownServer(serverHandle, 60, NULL);
    RfcDestroyServer(serverHandle, NULL);
  }

  if (client_conn_handle != NULL) {
    RfcCloseConnection(client_conn_handle, NULL);
  }
}

void Server::LockMutex() {
  // uv_sem_wait(&invocationMutex);
}

void Server::UnlockMutex() {
  // uv_sem_post(&invocationMutex);
}

}  // namespace node_rfc

void ServerCallJs(Napi::Env env,
                  Napi::Function callback,
                  std::nullptr_t* context,
                  ServerRequestBaton* requestBaton) {
  DEBUG("[NODE RFC] ServerCallJs begin\n");

  UNUSED(context);

  RFC_ERROR_INFO errorInfo;

  // set server context
  Napi::Value requestContext = node_rfc::getServerRequestContext(
      requestBaton->request_connection_handle, &errorInfo);

  //
  // ABAP -> JS parameters
  //
  DEBUG("[NODE RFC] ServerCallJs ABAP -> JS parameters\n");

  node_rfc::ValuePair jsParameters =
      getRfmParameters(requestBaton->func_desc_handle,
                       requestBaton->func_handle,
                       &requestBaton->errorPath,
                       &requestBaton->client_options);

  Napi::Value errorObj = jsParameters.first.As<Napi::Object>();
  Napi::Object abapArgs = jsParameters.second.As<Napi::Object>();

  if (!errorObj.IsUndefined()) {
    // todo
  }

  // set parameter count and names in request baton
  RfcGetParameterCount(requestBaton->func_desc_handle,
                       &requestBaton->paramCount,
                       requestBaton->errorInfo);
  requestBaton->paramNames = Napi::Persistent(abapArgs.GetPropertyNames());

  DEBUG("[NODE RFC] callback.Call() begin\n");
  Napi::Value result = callback.Call({requestContext, abapArgs});
  DEBUG("[NODE RFC] callback.Call() end\n");

  //
  // JS -> ABAP parameters
  //

  Napi::Object params = result.ToObject();
  Napi::Array paramNames = params.GetPropertyNames();
  uint_t paramSize = paramNames.Length();

  DEBUG(
      "[NODE RFC] ServerDoneCallback JS -> ABAP parameters: ", paramSize, "\n");

  errorObj = env.Undefined();
  for (uint_t i = 0; i < paramSize; i++) {
    Napi::String name = paramNames.Get(i).ToString();
    Napi::Value value = params.Get(name);
    printf("\n%s", value.ToString().Utf8Value().c_str());

    errorObj = node_rfc::setRfmParameter(requestBaton->func_desc_handle,
                                         requestBaton->func_handle,
                                         name,
                                         value,
                                         &requestBaton->errorPath,
                                         &requestBaton->client_options);

    if (!errorObj.IsUndefined()) {
      break;
    }
  }

  requestBaton->done();

  DEBUG("[NODE RFC] callback.Call() end, paramSize: ", paramSize, "\n");
  DEBUG("[NODE RFC] ServerCallJs end\n");
}
