/**
* Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH. and Contributors.
* SPDX-License-Identifier:	BSL-1.0
*
*/
#include <Common/NIO/SocketReactor.h>
#include <Common/NIO/SocketNotification.h>
#include <Common/NIO/SocketNotifier.h>
#include "Poco/ErrorHandler.h"
#include "Poco/Thread.h"
#include "Poco/Exception.h"


using Poco::Exception;
using Poco::Thread;
using Poco::ErrorHandler;
using Poco::Net::SocketImpl;


namespace RK {


SocketReactor::SocketReactor():
    _stop(false),
    _timeout(DEFAULT_TIMEOUT),
    _pReadableNotification(new ReadableNotification(this)),
    _pWritableNotification(new WritableNotification(this)),
    _pErrorNotification(new ErrorNotification(this)),
    _pTimeoutNotification(new TimeoutNotification(this)),
    _pIdleNotification(new IdleNotification(this)),
    _pShutdownNotification(new ShutdownNotification(this)),
    _pThread(nullptr)
{
}


SocketReactor::SocketReactor(const Poco::Timespan& timeout):
    _stop(false),
    _timeout(timeout),
    _pReadableNotification(new ReadableNotification(this)),
    _pWritableNotification(new WritableNotification(this)),
    _pErrorNotification(new ErrorNotification(this)),
    _pTimeoutNotification(new TimeoutNotification(this)),
    _pIdleNotification(new IdleNotification(this)),
    _pShutdownNotification(new ShutdownNotification(this)),
    _pThread(nullptr)
{
}


SocketReactor::~SocketReactor()
{
}


void SocketReactor::run()
{
    _pThread = Thread::current();
    while (!_stop)
    {
        try
        {
            if (!hasSocketHandlers())
            {
                onIdle();
                Thread::trySleep(static_cast<long>(_timeout.totalMilliseconds()));
            }
            else
            {
                bool readable = false;
                PollSet::SocketModeMap sm = _pollSet.poll(_timeout);
                if (sm.size() > 0)
                {
                    onBusy();
                    PollSet::SocketModeMap::iterator it = sm.begin();
                    PollSet::SocketModeMap::iterator end = sm.end();
                    for (; it != end; ++it)
                    {
                        if (it->second & PollSet::POLL_READ)
                        {
                            dispatch(it->first, _pReadableNotification);
                            readable = true;
                        }
                        if (it->second & PollSet::POLL_WRITE) dispatch(it->first, _pWritableNotification);
                        if (it->second & PollSet::POLL_ERROR) dispatch(it->first, _pErrorNotification);
                    }
                }
                if (!readable) onTimeout();
            }
        }
        catch (Exception& exc)
        {
            ErrorHandler::handle(exc);
        }
        catch (std::exception& exc)
        {
            ErrorHandler::handle(exc);
        }
        catch (...)
        {
            ErrorHandler::handle();
        }
    }
    onShutdown();
}


bool SocketReactor::hasSocketHandlers()
{
    if (!_pollSet.empty())
    {
        ScopedLock lock(_mutex);
        for (auto& p: _handlers)
        {
            if (p.second->accepts(_pReadableNotification) ||
                p.second->accepts(_pWritableNotification) ||
                p.second->accepts(_pErrorNotification)) return true;
        }
    }

    return false;
}


void SocketReactor::stop()
{
    _stop = true;
    wakeUp();
}

/// Wake up reactor, if invoker running in event loop thread there is no need to wake up.
void SocketReactor::wakeUp()
{
    auto * copy = _pThread.load(); /// to avoid data race
    if (copy && copy != Thread::current())
    {
        copy->wakeUp();
        _pollSet.wakeUp();
    }
}


void SocketReactor::setTimeout(const Poco::Timespan& timeout)
{
    _timeout = timeout;
}


const Poco::Timespan& SocketReactor::getTimeout() const
{
    return _timeout;
}


void SocketReactor::addEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
    NotifierPtr pNotifier = getNotifier(socket, true);

    if (!pNotifier->hasObserver(observer)) pNotifier->addObserver(this, observer);

    int mode = 0;
    if (pNotifier->accepts(_pReadableNotification)) mode |= PollSet::POLL_READ;
    if (pNotifier->accepts(_pWritableNotification)) mode |= PollSet::POLL_WRITE;
    if (pNotifier->accepts(_pErrorNotification))    mode |= PollSet::POLL_ERROR;
    if (mode) _pollSet.add(socket, mode);
}

void SocketReactor::addEventHandlers(const Socket& socket, const std::vector<Poco::AbstractObserver *>& observers)
{
    NotifierPtr pNotifier = getNotifier(socket, true);

    for (auto * observer : observers)
    {
        if (!pNotifier->hasObserver(*observer)) pNotifier->addObserver(this, *observer);

        int mode = 0;
        if (pNotifier->accepts(_pReadableNotification)) mode |= PollSet::POLL_READ;
        if (pNotifier->accepts(_pWritableNotification)) mode |= PollSet::POLL_WRITE;
        if (pNotifier->accepts(_pErrorNotification))    mode |= PollSet::POLL_ERROR;
        if (mode) _pollSet.add(socket, mode);
    }
}


bool SocketReactor::hasEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
    NotifierPtr pNotifier = getNotifier(socket);
    if (!pNotifier) return false;
    if (pNotifier->hasObserver(observer)) return true;
    return false;
}


SocketReactor::NotifierPtr SocketReactor::getNotifier(const Socket& socket, bool makeNew)
{
    const SocketImpl* pImpl = socket.impl();
    if (pImpl == nullptr) return nullptr;
    poco_socket_t sockfd = pImpl->sockfd();
    ScopedLock lock(_mutex);

    EventHandlerMap::iterator it = _handlers.find(sockfd);
    if (it != _handlers.end()) return it->second;
    else if (makeNew) return (_handlers[sockfd] = new SocketNotifier(socket));

    return nullptr;
}


void SocketReactor::removeEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
    const SocketImpl* pImpl = socket.impl();
    if (pImpl == nullptr) return;
    NotifierPtr pNotifier = getNotifier(socket);
    if (pNotifier && pNotifier->hasObserver(observer))
    {
        if(pNotifier->countObservers() == 1)
        {
            {
                ScopedLock lock(_mutex);
                _handlers.erase(pImpl->sockfd());
            }
            _pollSet.remove(socket);
        }
        pNotifier->removeObserver(this, observer);

        if (pNotifier->countObservers() > 0 && socket.impl()->sockfd() > 0)
        {
            int mode = 0;
            if (pNotifier->accepts(_pReadableNotification)) mode |= PollSet::POLL_READ;
            if (pNotifier->accepts(_pWritableNotification)) mode |= PollSet::POLL_WRITE;
            if (pNotifier->accepts(_pErrorNotification))    mode |= PollSet::POLL_ERROR;
            _pollSet.update(socket, mode);
        }
    }
}


bool SocketReactor::has(const Socket& socket) const
{
    return _pollSet.has(socket);
}


void SocketReactor::onTimeout()
{
    dispatch(_pTimeoutNotification);
}


void SocketReactor::onIdle()
{
    dispatch(_pIdleNotification);
}


void SocketReactor::onShutdown()
{
    dispatch(_pShutdownNotification);
}


void SocketReactor::onBusy()
{
}


void SocketReactor::dispatch(const Socket& socket, SocketNotification* pNotification)
{
    NotifierPtr pNotifier = getNotifier(socket);
    if (!pNotifier) return;
    dispatch(pNotifier, pNotification);
}


void SocketReactor::dispatch(SocketNotification* pNotification)
{
    std::vector<NotifierPtr> delegates;
    {
        ScopedLock lock(_mutex);
        delegates.reserve(_handlers.size());
        for (EventHandlerMap::iterator it = _handlers.begin(); it != _handlers.end(); ++it)
            delegates.push_back(it->second);
    }
    for (std::vector<NotifierPtr>::iterator it = delegates.begin(); it != delegates.end(); ++it)
    {
        dispatch(*it, pNotification);
    }
}


void SocketReactor::dispatch(NotifierPtr& pNotifier, SocketNotification* pNotification)
{
    try
    {
        pNotifier->dispatch(pNotification);
    }
    catch (Exception& exc)
    {
        ErrorHandler::handle(exc);
    }
    catch (std::exception& exc)
    {
        ErrorHandler::handle(exc);
    }
    catch (...)
    {
        ErrorHandler::handle();
    }
}


}
