/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include <com/com.h>
#include <core/core.h>

#include <displayinfo.h>
#include <interfaces/IDisplayInfo.h>

#ifndef __DEBUG__
#define Trace(fmt, ...)                                                                                                                     \
    do {                                                                                                                                    \
        fprintf(stdout, "\033[1;32m[%s:%d](%s){%p}<%d>:" fmt "\n\033[0m", __FILE__, __LINE__, __FUNCTION__, this, getpid(), ##__VA_ARGS__); \
        fflush(stdout);                                                                                                                     \
    } while (0)
#else
#define Trace(fmt, ...)
#endif

namespace WPEFramework {
class DisplayInfo : public Core::IReferenceCounted {
private:
    typedef std::map<displayinfo_updated_cb, void*> Callbacks;

    class Notification : public Exchange::IConnectionProperties::INotification {
    public:
        Notification() = delete;
        Notification(const Notification&) = delete;
        Notification& operator=(const Notification&) = delete;

        Notification(DisplayInfo* parent)
            : _parent(*parent)
        {
        }

        void Updated() override
        {
            _parent.Updated();
        }

        BEGIN_INTERFACE_MAP(Notification)
        INTERFACE_ENTRY(Exchange::IConnectionProperties::INotification)
        END_INTERFACE_MAP

    private:
        DisplayInfo& _parent;
    };

    #ifdef __WINDOWS__
    #pragma warning(disable : 4355)
    #endif
    DisplayInfo(const string& displayName, Exchange::IConnectionProperties* interface)
        : _refCount(1)
        , _name(displayName)
        , _displayConnection(interface)
        , _notification(this)
        , _callbacks()
    {
        _displayConnection->Register(&_notification);
    }
    #ifdef __WINDOWS__
    #pragma warning(default : 4355)
    #endif

    class DisplayInfoAdministration : protected std::list<DisplayInfo*> {
    public:
        DisplayInfoAdministration(const DisplayInfoAdministration&) = delete;
        DisplayInfoAdministration& operator=(const DisplayInfoAdministration&) = delete;

        DisplayInfoAdministration()
            : _adminLock()
            , _engine(Core::ProxyType<RPC::InvokeServerType<1, 0, 4>>::Create())
        {
        }

        ~DisplayInfoAdministration()
        {
            std::list<DisplayInfo*>::iterator index(std::list<DisplayInfo*>::begin());

            while (index != std::list<DisplayInfo*>::end()) {
                Trace("Closing %s", (*index)->Name().c_str());
            }

            ASSERT(std::list<DisplayInfo*>::size() == 0);
        }

        DisplayInfo* Instance(const string& name)
        {
            DisplayInfo* result(nullptr);

            _adminLock.Lock();

            result = Find(name);

            if (result == nullptr) {

                Exchange::IConnectionProperties* displayInterface = GetInterface<Exchange::IConnectionProperties>(name);

                if (displayInterface != nullptr) {
                    result = new DisplayInfo(name, displayInterface);
                    std::list<DisplayInfo*>::emplace_back(result);
                }
            }
            _adminLock.Unlock();

            return (result);
        }

        uint32_t Delete(const DisplayInfo* displayInfo, int& refCount)
        {
            uint32_t result(Core::ERROR_NONE);

            _adminLock.Lock();

            if (Core::InterlockedDecrement(refCount) == 0) {
                std::list<DisplayInfo*>::iterator index(
                    std::find(std::list<DisplayInfo*>::begin(), std::list<DisplayInfo*>::end(), displayInfo));

                ASSERT(index != std::list<DisplayInfo*>::end());

                if (index != std::list<DisplayInfo*>::end()) {
                    std::list<DisplayInfo*>::erase(index);
                }
                delete const_cast<DisplayInfo*>(displayInfo);
                result = Core::ERROR_DESTRUCTION_SUCCEEDED;
            }

            _adminLock.Unlock();

            return result;
        }

        uint8_t Enumerate(std::vector<string>& instances)
        {
            class Catalog : protected PluginHost::IPlugin::INotification {
            public:
                Catalog(const Catalog&) = delete;
                Catalog& operator=(const Catalog&) = delete;

                Catalog() = default;
                ~Catalog() override = default;

                void Load(PluginHost::IShell* systemInterface, std::vector<string>& modules)
                {
                    ASSERT(_instances.size() == 0);

                    systemInterface->Register(this);
                    systemInterface->Unregister(this);
  
                    while (_instances.size() > 0) {

                        PluginHost::IShell* current = _instances.back();
                        Exchange::IConnectionProperties* props = current->QueryInterface<Exchange::IConnectionProperties>();

                        if (props != nullptr) {
                            modules.push_back(current->Callsign());
                            props->Release();
                        }
                        current->Release();
                        _instances.pop_back();
                    }
                }

            private:
                void StateChange(PluginHost::IShell* plugin) override
                {
                    plugin->AddRef();
                    _instances.push_back(plugin);
                }

                BEGIN_INTERFACE_MAP(Catalog)
                INTERFACE_ENTRY(PluginHost::IPlugin::INotification)
                END_INTERFACE_MAP

            private:
                std::vector<PluginHost::IShell*> _instances;
            };
            PluginHost::IShell* systemInterface = GetInterface<PluginHost::IShell>(string());
            if (systemInterface != nullptr) {
                Core::Sink<Catalog> mySink;
                mySink.Load(systemInterface, instances);
                systemInterface->Release();
            }

            return static_cast<uint8_t>(instances.size());
        }

    private:
        DisplayInfo* Find(const string& name)
        {
            DisplayInfo* result(nullptr);

            std::list<DisplayInfo*>::iterator index(std::list<DisplayInfo*>::begin());

            while ((index != std::list<DisplayInfo*>::end()) && ((*index)->Name() != name)) {
                index++;
            }

            if (index != std::list<DisplayInfo*>::end()) {
                result = *index;
                result->AddRef();
            }

            return result;
        }

        template <typename INTERFACE>
        INTERFACE* GetInterface(const string& name)
        {
            const TCHAR* comPath = ::getenv(_T("COMMUNICATOR_PATH"));

            if (comPath == nullptr) {
#ifdef __WINDOWS__
                comPath = _T("127.0.0.1:62000");
#else
                comPath = _T("/tmp/communicator");
#endif
            }

            Core::ProxyType<RPC::CommunicatorClient> comChannel(Core::ProxyType<RPC::CommunicatorClient>::Create(Core::NodeId(comPath), _engine));

            return comChannel->Open<INTERFACE>(name, ~0, RPC::CommunicationTimeOut);
        }

        Core::CriticalSection _adminLock;
        Core::ProxyType<Core::IIPCServer> _engine;
    };

    ~DisplayInfo()
    {
        _displayConnection->Unregister(&_notification);
    }

public:
    DisplayInfo() = delete;
    DisplayInfo(const DisplayInfo&) = delete;
    DisplayInfo& operator=(const DisplayInfo&) = delete;

    static bool Enumerate(const uint8_t index, const uint8_t length, char* buffer)
    {

        static std::vector<string> interfaces;
        static Core::CriticalSection interfacesLock;
        bool result(false);

        interfacesLock.Lock();

        if (index == 0) {
            interfaces.clear();
            _administration.Enumerate(interfaces);
        }

        if (index < interfaces.size()) {
            if (length > 0) {
                ASSERT(buffer != nullptr);
                ::strncpy(buffer, interfaces[index].c_str(), length);
            }

            result = true;
        }

        interfacesLock.Unlock();

        return result;
    }
    static DisplayInfo* Instance(const string& displayName)
    {
        return _administration.Instance(displayName);
    }

    void AddRef() const
    {
        Core::InterlockedIncrement(_refCount);
    }
    uint32_t Release() const
    {
        return _administration.Delete(this, _refCount);
    }

    void Updated()
    {
        Callbacks::iterator index(_callbacks.begin());

        while (index != _callbacks.end()) {
            index->first(reinterpret_cast<displayinfo_type*>(this), index->second);
            index++;
        }
    }
    void Register(displayinfo_updated_cb callback, void* userdata)
    {
        Callbacks::iterator index(_callbacks.find(callback));

        if (index == _callbacks.end()) {
            _callbacks.emplace(std::piecewise_construct,
                std::forward_as_tuple(callback),
                std::forward_as_tuple(userdata));
        }
    }
    void Unregister(displayinfo_updated_cb callback)
    {
        Callbacks::iterator index(_callbacks.find(callback));

        if (index != _callbacks.end()) {
            _callbacks.erase(index);
        }
    }

    const string& Name() const { return _name; }

    bool IsAudioPassthrough() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->IsAudioPassthrough();
    }
    bool Connected() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->Connected();
    }
    uint32_t Width() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->Width();
    }
    uint32_t Height() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->Height();
    }
    uint32_t VerticalFreq() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->VerticalFreq();
    }
    Exchange::IConnectionProperties::HDRType HDR() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->Type();
    }
    Exchange::IConnectionProperties::HDCPProtectionType HDCPProtection() const
    {
        ASSERT(_displayConnection != nullptr);
        return _displayConnection->HDCPProtection();
    }

private:
    mutable int _refCount;
    const string _name;
    Exchange::IConnectionProperties* _displayConnection;
    Core::Sink<Notification> _notification;
    Callbacks _callbacks;
    static DisplayInfo::DisplayInfoAdministration _administration;
};

/* static */ DisplayInfo::DisplayInfoAdministration DisplayInfo::_administration;

} // namespace WPEFramework

using namespace WPEFramework;

extern "C" {
bool displayinfo_enumerate(const uint8_t index, const uint8_t length, char* buffer)
{

    return DisplayInfo::Enumerate(index, length, buffer);
}

struct displayinfo_type* displayinfo_instance(const char displayName[] = "DisplayInfo")
{
    return reinterpret_cast<displayinfo_type*>(DisplayInfo::Instance(string(displayName)));
}

void displayinfo_release(struct displayinfo_type* displayinfo)
{
    reinterpret_cast<DisplayInfo*>(displayinfo)->Release();
}

void displayinfo_register(struct displayinfo_type* displayinfo, displayinfo_updated_cb callback, void* userdata)
{
    reinterpret_cast<DisplayInfo*>(displayinfo)->Register(callback, userdata);
}

void displayinfo_unregister(struct displayinfo_type* displayinfo, displayinfo_updated_cb callback)
{
    reinterpret_cast<DisplayInfo*>(displayinfo)->Unregister(callback);
}

void displayinfo_name(struct displayinfo_type* displayinfo, char buffer[], const uint8_t length)
{
    string name = reinterpret_cast<DisplayInfo*>(displayinfo)->Name();
    strncpy(buffer, name.c_str(), length);
}

bool displayinfo_is_audio_passthrough(struct displayinfo_type* displayinfo)
{
    return reinterpret_cast<DisplayInfo*>(displayinfo)->IsAudioPassthrough();
}

bool displayinfo_connected(struct displayinfo_type* displayinfo)
{
    return reinterpret_cast<DisplayInfo*>(displayinfo)->Connected();
}

uint32_t displayinfo_width(struct displayinfo_type* displayinfo)
{
    return reinterpret_cast<DisplayInfo*>(displayinfo)->Width();
}

uint32_t displayinfo_height(struct displayinfo_type* displayinfo)
{
    return reinterpret_cast<DisplayInfo*>(displayinfo)->Height();
}

uint32_t displayinfo_vertical_frequency(struct displayinfo_type* displayinfo)
{
    return reinterpret_cast<DisplayInfo*>(displayinfo)->VerticalFreq();
}

displayinfo_hdr_t displayinfo_hdr(struct displayinfo_type* displayinfo)
{
    displayinfo_hdr_t result = DISPLAYINFO_HDR_UNKNOWN;

    switch (reinterpret_cast<DisplayInfo*>(displayinfo)->HDR()) {
    case Exchange::IConnectionProperties::HDR_OFF:
        result = DISPLAYINFO_HDR_OFF;
        break;
    case Exchange::IConnectionProperties::HDR_10:
        result = DISPLAYINFO_HDR_10;
        break;
    case Exchange::IConnectionProperties::HDR_10PLUS:
        result = DISPLAYINFO_HDR_10PLUS;
        break;
    case Exchange::IConnectionProperties::HDR_DOLBYVISION:
        result = DISPLAYINFO_HDR_DOLBYVISION;
        break;
    case Exchange::IConnectionProperties::HDR_TECHNICOLOR:
        result = DISPLAYINFO_HDR_TECHNICOLOR;
        break;
    default:
        result = DISPLAYINFO_HDR_UNKNOWN;
        break;
    }

    return result;
}

displayinfo_hdcp_protection_t displayinfo_hdcp_protection(struct displayinfo_type* displayinfo)
{

    displayinfo_hdcp_protection_t type = DISPLAYINFO_HDCP_UNKNOWN;

    switch (reinterpret_cast<DisplayInfo*>(displayinfo)->HDCPProtection()) {
    case Exchange::IConnectionProperties::HDCP_Unencrypted:
        type = DISPLAYINFO_HDCP_UNENCRYPTED;
        break;
    case Exchange::IConnectionProperties::HDCP_1X:
        type = DISPLAYINFO_HDCP_1X;
        break;
    case Exchange::IConnectionProperties::HDCP_2X:
        type = DISPLAYINFO_HDCP_2X;
        break;
    default:
        type = DISPLAYINFO_HDCP_UNKNOWN;
        break;
    }

    return type;
}

} // extern "C"
