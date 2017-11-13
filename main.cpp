#include "moonlight.hpp"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <pairing.h>

#include "ppapi/cpp/input_event.h"

#include <netinet/in.h>

// Requests the NaCl module to connection to the server specified after the :
#define MSG_START_REQUEST "startRequest"
// Requests the NaCl module stop streaming
#define MSG_STOP_REQUEST "stopRequest"
// Sent by the NaCl module when the stream has stopped whether user-requested or not
#define MSG_STREAM_TERMINATED "streamTerminated"

#define MSG_OPENURL "openUrl"

MoonlightInstance* g_Instance;

class MoonlightModule : public pp::Module {
    public:
        MoonlightModule() : pp::Module() {}
        virtual ~MoonlightModule() {}

        virtual pp::Instance* CreateInstance(PP_Instance instance) {
            return new MoonlightInstance(instance);
        }
};

void MoonlightInstance::OnConnectionStarted(uint32_t unused) {
    // Tell the front end
    pp::Var response("Connection Established");
    PostMessage(response);
    
    // Start receiving input events
    //RequestInputEvents(PP_INPUTEVENT_CLASS_MOUSE | PP_INPUTEVENT_CLASS_WHEEL);
    
    // Filtering is suboptimal but it ensures that we can pass keyboard events
    // to the browser when mouse lock is disabled. This is neccessary for Esc
    // to kick the app out of full-screen.
    //RequestFilteringInputEvents(PP_INPUTEVENT_CLASS_KEYBOARD);
}

void MoonlightInstance::OnConnectionStopped(uint32_t error) {
    // Not running anymore
    m_Running = false;
    
    // Stop receiving input events
    //ClearInputEventRequest(PP_INPUTEVENT_CLASS_MOUSE | PP_INPUTEVENT_CLASS_WHEEL | PP_INPUTEVENT_CLASS_KEYBOARD);
    
    // Unlock the mouse
    UnlockMouse();
    
    // Notify the JS code that the stream has ended
    pp::Var response(MSG_STREAM_TERMINATED);
    PostMessage(response);
}

void MoonlightInstance::StopConnection() {
    pthread_t t;
    
    // Stopping needs to happen in a separate thread to avoid a potential deadlock
    // caused by us getting a callback to the main thread while inside LiStopConnection.
    pthread_create(&t, NULL, MoonlightInstance::StopThreadFunc, NULL);
    
    // We'll need to call the listener ourselves since our connection terminated callback
    // won't be invoked for a manually requested termination.
    OnConnectionStopped(0);
}

void* MoonlightInstance::StopThreadFunc(void* context) {
    // We must join the connection thread first, because LiStopConnection must
    // not be invoked during LiStartConnection.
    pthread_join(g_Instance->m_ConnectionThread, NULL);

    // Not running anymore
    g_Instance->m_Running = false;

    // We also need to stop this thread after the connection thread, because it depends
    // on being initialized there.
    //pthread_join(g_Instance->m_InputThread, NULL);

    // Stop the connection
    LiStopConnection();
    return NULL;
}

void* MoonlightInstance::InputThreadFunc(void* context) {
    MoonlightInstance* me = (MoonlightInstance*)context;

    while (me->m_Running) {
        me->PollGamepads();
        me->ReportMouseMovement();
        
        // Poll every 10 ms
        usleep(10 * 1000);
    }
    
    return NULL;
}

void* MoonlightInstance::ConnectionThreadFunc(void* context) {
    MoonlightInstance* me = (MoonlightInstance*)context;
    int err;
    SERVER_INFORMATION serverInfo;
    
    // Post a status update before we begin
    pp::Var response("Starting connection to " + me->m_Host);
    me->PostMessage(response);
    
    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = me->m_Host.c_str();
    serverInfo.serverInfoAppVersion = me->m_AppVersion.c_str();
    
    err = LiStartConnection(&serverInfo,
                            &me->m_StreamConfig,
                            &MoonlightInstance::s_ClCallbacks,
                            &MoonlightInstance::s_DrCallbacks,
                            &MoonlightInstance::s_ArCallbacks,
                            NULL, 0,
                            NULL, 0);
    if (err != 0) {
        // Notify the JS code that the stream has ended
        pp::Var response(MSG_STREAM_TERMINATED);
        me->PostMessage(response);
        return NULL;
    }
    
    // Set running state before starting connection-specific threads
    me->m_Running = true;
    
    //pthread_create(&me->m_InputThread, NULL, MoonlightInstance::InputThreadFunc, me);
    
    return NULL;
}

// hook from javascript into the CPP code.
void MoonlightInstance::HandleMessage(const pp::Var& var_message) {
     // Ignore the message if it is not a string.
    if (!var_message.is_dictionary())
        return;
    
    pp::VarDictionary msg(var_message);
    int32_t callbackId = msg.Get("callbackId").AsInt();
    std::string method = msg.Get("method").AsString();
    pp::VarArray params(msg.Get("params"));
    
    if (strcmp(method.c_str(), MSG_START_REQUEST) == 0) {
        HandleStartStream(callbackId, params);
    } else if (strcmp(method.c_str(), MSG_STOP_REQUEST) == 0) {
        HandleStopStream(callbackId, params);
    } else if (strcmp(method.c_str(), MSG_OPENURL) == 0) {
        HandleOpenURL(callbackId, params);
    } else if (strcmp(method.c_str(), "httpInit") == 0) {
        NvHTTPInit(callbackId, params);
    } else if (strcmp(method.c_str(), "makeCert") == 0) {
        MakeCert(callbackId, params);
    } else if (strcmp(method.c_str(), "pair") == 0) {
        HandlePair(callbackId, params);
    } else {
        pp::Var response("Unhandled message received: " + method);
        PostMessage(response);
    }
}

static void hexStringToBytes(const char* str, char* output) {
    for (int i = 0; i < strlen(str); i += 2) {
        sscanf(&str[i], "%2hhx", &output[i / 2]);
    }
}

void MoonlightInstance::HandleStartStream(int32_t callbackId, pp::VarArray args) {
    std::string host = args.Get(0).AsString();
    std::string width = args.Get(1).AsString();
    std::string height = args.Get(2).AsString();
    std::string fps = args.Get(3).AsString();
    std::string bitrate = args.Get(4).AsString();
    std::string rikey = args.Get(5).AsString();
    std::string rikeyid = args.Get(6).AsString();
    std::string appversion = args.Get(7).AsString();
    
    pp::Var response("Setting stream width to: " + width);
    PostMessage(response);
    response = ("Setting stream height to: " + height);
    PostMessage(response);
    response = ("Setting stream fps to: " + fps);
    PostMessage(response);
    response = ("Setting stream host to: " + host);
    PostMessage(response);
    response = ("Setting stream bitrate to: " + bitrate);
    PostMessage(response);
    response = ("Setting rikey to: " + rikey);
    PostMessage(response);
    response = ("Setting rikeyid to: " + rikeyid);
    PostMessage(response);
    response = ("Setting appversion to: " + appversion);
    PostMessage(response);
    
    // Populate the stream configuration
    LiInitializeStreamConfiguration(&m_StreamConfig);
    m_StreamConfig.width = stoi(width);
    m_StreamConfig.height = stoi(height);
    m_StreamConfig.fps = stoi(fps);
    m_StreamConfig.bitrate = stoi(bitrate); // kilobits per second
    m_StreamConfig.streamingRemotely = 0;
    m_StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    
    // The overhead of receiving a packet is much higher in NaCl because we must
    // pass through various layers of abstraction on each recv() call. We're using a
    // higher than normal default video packet size here to reduce CPU cycles wasted
    // receiving packets. The possible cost is greater network losses.
    m_StreamConfig.packetSize = 1392;
    
    // Load the rikey and rikeyid into the stream configuration
    hexStringToBytes(rikey.c_str(), m_StreamConfig.remoteInputAesKey);
    int rikeyiv = htonl(stoi(rikeyid));
    memcpy(m_StreamConfig.remoteInputAesIv, &rikeyiv, sizeof(rikeyiv));

    // Store the parameters from the start message
    m_Host = host;
    m_AppVersion = appversion;
    
    // Initialize the rendering surface before starting the connection
    if (InitializeRenderingSurface(m_StreamConfig.width, m_StreamConfig.height)) {
        // Start the worker thread to establish the connection
        pthread_create(&m_ConnectionThread, NULL, MoonlightInstance::ConnectionThreadFunc, this);
    } else {
        // Failed to initialize renderer
        OnConnectionStopped(0);
    }
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::VarDictionary());
    PostMessage(ret);
}

void MoonlightInstance::HandleStopStream(int32_t callbackId, pp::VarArray args) {
    // Begin connection teardown
    StopConnection();
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::VarDictionary());
    PostMessage(ret);
}

void MoonlightInstance::HandleOpenURL(int32_t callbackId, pp::VarArray args) {
    std::string url = args.Get(0).AsString();
    bool binaryResponse = args.Get(1).AsBool();
    
    m_HttpThreadPool[m_HttpThreadPoolSequence++ % HTTP_HANDLER_THREADS]->message_loop().PostWork(
        m_CallbackFactory.NewCallback(&MoonlightInstance::NvHTTPRequest, callbackId, url, binaryResponse));
    
    PostMessage(pp::Var (url.c_str()));
}

void MoonlightInstance::HandlePair(int32_t callbackId, pp::VarArray args) {
     m_HttpThreadPool[m_HttpThreadPoolSequence++ % HTTP_HANDLER_THREADS]->message_loop().PostWork(
         m_CallbackFactory.NewCallback(&MoonlightInstance::PairCallback, callbackId, args));
}

void MoonlightInstance::PairCallback(int32_t /*result*/, int32_t callbackId, pp::VarArray args) {
    int err = gs_pair(atoi(args.Get(0).AsString().c_str()), args.Get(1).AsString().c_str(), args.Get(2).AsString().c_str());
    
    pp::VarDictionary ret;
    ret.Set("callbackId", pp::Var(callbackId));
    ret.Set("type", pp::Var("resolve"));
    ret.Set("ret", pp::Var(err));
    PostMessage(ret);
}

bool MoonlightInstance::Init(uint32_t argc,
                             const char* argn[],
                             const char* argv[]) {
    g_Instance = this;
    return true;
}

namespace pp {
Module* CreateModule() {
    return new MoonlightModule();
}
}  // namespace pp
