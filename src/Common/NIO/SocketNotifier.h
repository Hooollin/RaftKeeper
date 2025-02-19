/**
* Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH. and Contributors.
* SPDX-License-Identifier:	BSL-1.0
*
*/
#pragma once

#include "Poco/Net/Net.h"
#include "Poco/Net/Socket.h"
#include "Poco/RefCountedObject.h"
#include "Poco/NotificationCenter.h"
#include "Poco/Observer.h"
#include <set>

namespace RK {

class SocketReactor;
class SocketNotification;

using Poco::Net::Socket;


class Net_API SocketNotifier: public Poco::RefCountedObject
/// This class is used internally by SocketReactor
/// to notify registered event handlers of socket events.
{
public:
    explicit SocketNotifier(const Socket& socket);
    /// Creates the SocketNotifier for the given socket.

    void addObserver(SocketReactor* pReactor, const Poco::AbstractObserver& observer);
    /// Adds the given observer.

    void removeObserver(SocketReactor* pReactor, const Poco::AbstractObserver& observer);
    /// Removes the given observer.

    bool hasObserver(const Poco::AbstractObserver& observer) const;
    /// Returns true if the given observer is registered.

    bool accepts(SocketNotification* pNotification);
    /// Returns true if there is at least one observer for the given notification.

    void dispatch(SocketNotification* pNotification);
    /// Dispatches the notification to all observers.

    bool hasObservers() const;
    /// Returns true if there are subscribers.

    std::size_t countObservers() const;
    /// Returns the number of subscribers;

protected:
    ~SocketNotifier() override;
    /// Destroys the SocketNotifier.

private:
    typedef std::multiset<SocketNotification*> EventSet;
    typedef Poco::FastMutex                    MutexType;
    typedef MutexType::ScopedLock              ScopedLock;

    EventSet                 _events;
    Poco::NotificationCenter _nc;
    Socket                   _socket;
    MutexType                _mutex;
};


//
// inlines
//
inline bool SocketNotifier::accepts(SocketNotification* pNotification)
{
    ScopedLock l(_mutex);
    return _events.find(pNotification) != _events.end();
}


inline bool SocketNotifier::hasObserver(const Poco::AbstractObserver& observer) const
{
    return _nc.hasObserver(observer);
}


inline bool SocketNotifier::hasObservers() const
{
    return _nc.hasObservers();
}


inline std::size_t SocketNotifier::countObservers() const
{
    return _nc.countObservers();
}


}
