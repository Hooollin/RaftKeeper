#pragma once

#include <type_traits>
#include <Poco/NObserver.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/SharedPtr.h>
#include <Poco/Thread.h>
#include <Common/setThreadName.h>
#include <Poco/Net/SocketNotification.h>
#include <Poco/Net/SocketReactor.h>


namespace RK {

    using namespace Poco::Net;

    template <class SR, typename = typename std::enable_if<std::is_base_of<SocketReactor, SR>::value>::type>
    class SvsSocketReactor : public SR
    {
    public:
        using Ptr = Poco::SharedPtr<SvsSocketReactor>;

        explicit SvsSocketReactor(const std::string& name = "")
        {
            thread_.start(*this);
            if (!name.empty())
                thread_.setName(name);
        }

        explicit SvsSocketReactor(const Poco::Timespan& timeout, const std::string& name = ""):
            SR(timeout)
        {
            thread_.start(*this);
            if (!name.empty())
                thread_.setName(name);
        }

        ~SvsSocketReactor() override
        {
            try
            {
                this->stop();
                thread_.join();
            }
            catch (...)
            {
            }
        }
	
    protected:
        void onIdle() override
        {
            SR::onIdle();
            Poco::Thread::yield();
        }
	
    private:
        Poco::Thread thread_;
    };

}
