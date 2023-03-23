/**
* Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH. and Contributors.
* SPDX-License-Identifier:	BSL-1.0
*
*/
#pragma once

#include <Poco/Net/SocketReactor.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Environment.h>
#include <Poco/NObserver.h>
#include <Poco/SharedPtr.h>
#include <Poco/Timespan.h>
#include <memory>
#include <type_traits>
#include <vector>

#include <Service/Context.h>
#include <Common/NIO/SvsSocketReactor.h>


using Poco::Net::Socket;
using Poco::Net::ServerSocket;
using Poco::Net::StreamSocket;


namespace RK {

template <class ServiceHandler, class SR, typename = typename std::enable_if<std::is_base_of<SocketReactor, SR>::value>::type>
class SvsSocketAcceptor
  /// This class implements the Acceptor part of the Acceptor-Connector design pattern.
  /// Only the difference from single-threaded version is documented here, For full 
  /// description see Poco::Net::SocketAcceptor documentation.
  /// 
  /// This is a multi-threaded version of SocketAcceptor, it differs from the
  /// single-threaded version in number of reactors (defaulting to number of processors)
  /// that can be specified at construction time and is rotated in a round-robin fashion
  /// by event handler. See ParallelSocketAcceptor::onAccept and 
  /// ParallelSocketAcceptor::createServiceHandler documentation and implementation for 
  /// details.
  {
  public:
    using ParallelReactor = SvsSocketReactor<SR>;
    using Observer = Poco::Observer<SvsSocketAcceptor, ReadableNotification>;

    SvsSocketAcceptor(
      const String& name,
      Context & keeper_context,
      ServerSocket & socket,
      const Poco::Timespan & timeout,
      unsigned threads = Poco::Environment::processorCount())
      : name_(name)
      , socket_(socket)
      , threads_(threads)
      , next_(0)
      , keeper_context_(keeper_context)
      , timeout_(timeout)
    /// Creates a ParallelSocketAcceptor using the given ServerSocket, sets the
    /// number of threads, populates the reactors vector and registers itself 
    /// with the given SocketReactor.
    {
      init();
    }

    virtual ~SvsSocketAcceptor()
    /// Destroys the ParallelSocketAcceptor.
    {
      try
      {
        if (server_reactor_)
        {
          server_reactor_->removeEventHandler(socket_, Observer(*this, &SvsSocketAcceptor::onAccept));
        }
      }
      catch (...)
      {
      }
    }

    void setReactor(SocketReactor& reactor)
    /// Sets the reactor for this acceptor.
    {
      registerAcceptor(std::make_shared<SocketReactor>(reactor));
    }

    virtual void registerAcceptor(std::shared_ptr<SocketReactor> reactor)
    /// Registers the ParallelSocketAcceptor with a SocketReactor.
    ///
    /// A subclass can override this function to e.g.
    /// register an event handler for timeout event.
    /// 
    /// The overriding method must either call the base class
    /// implementation or register the accept handler on its own.
    {
      server_reactor_ = reactor;
      if (!server_reactor_->hasEventHandler(socket_, Observer(*this, &SvsSocketAcceptor::onAccept)))
      {
        server_reactor_->addEventHandler(socket_, Observer(*this, &SvsSocketAcceptor::onAccept));
        server_reactor_->run();
      }
    }

    virtual void unregisterAcceptor()
    /// Unregisters the ParallelSocketAcceptor.
    ///
    /// A subclass can override this function to e.g.
    /// unregister its event handler for a timeout event.
    /// 
    /// The overriding method must either call the base class
    /// implementation or unregister the accept handler on its own.
    {
      if (server_reactor_)
      {
        server_reactor_->removeEventHandler(socket_, Observer(*this, &SvsSocketAcceptor::onAccept));
      }
    }

    void onAccept(ReadableNotification* pNotification)
    /// Accepts connection and creates event handler.
    /// TODO why wait a moment?  For when adding EventHandler it does not wake up register.
    /// and need register event? no
    {
      pNotification->release();
      StreamSocket sock = socket_.acceptConnection();
      createServiceHandler(sock);
    }

    void stop() {
      if(server_reactor_) {
        server_reactor_->stop();
      }
    }

    SvsSocketAcceptor() = delete;
    SvsSocketAcceptor(const SvsSocketAcceptor &) = delete;
    SvsSocketAcceptor & operator = (const SvsSocketAcceptor &) = delete;

  protected:
    using ReactorVec = std::vector<typename ParallelReactor::Ptr>;

    virtual ServiceHandler* createServiceHandler(StreamSocket& socket)
    /// Create and initialize a new ServiceHandler instance.
    /// If socket is already registered with a reactor, the new
    /// ServiceHandler instance is given that reactor; otherwise,
    /// the next reactor is used. Reactors are rotated in round-robin
    /// fashion.
    ///
    /// Subclasses can override this method.
    {
      socket.setBlocking(false);
      SocketReactor* pReactor = reactor(socket);
      if (!pReactor)
      {
        std::size_t next = next_++;
        if (next_ == parallel_reactors_.size()) next_ = 0;
        pReactor = parallel_reactors_[next];
      }
      auto* ret = new ServiceHandler(keeper_context_, socket, *pReactor);
      pReactor->wakeUp();
      return ret;
    }

    SocketReactor* reactor(const Socket& socket)
    /// Returns reactor where this socket is already registered
    /// for polling, if found; otherwise returns null pointer.
    {
      typename ReactorVec::iterator it = parallel_reactors_.begin();
      typename ReactorVec::iterator end = parallel_reactors_.end();
      for (; it != end; ++it)
      {
        if ((*it)->has(socket)) return it->get();
      }
      return nullptr;
    }

    SocketReactor* reactor()
    /// Returns a pointer to the SocketReactor where
    /// this SocketAcceptor is registered.
    ///
    /// The pointer may be null.
    {
      return server_reactor_.get();
    }

    Socket& socket()
    /// Returns a reference to the SocketAcceptor's socket.
    {
      return socket_;
    }

    void init()
    /// Populates the reactors vector.
    {
      server_reactor_ = std::make_shared<SocketReactor>(timeout_);
      server_reactor_->addEventHandler(socket_, Observer(*this, &SvsSocketAcceptor::onAccept));
      /// It is necessary to wake up the reactor. or run? 
      server_reactor_->run();

      poco_assert (threads_ > 0);
      for (unsigned i = 0; i < threads_; ++i)
        parallel_reactors_.push_back(new ParallelReactor(timeout_, name_ + "#" + std::to_string(i)));
    }

    ReactorVec& reactors()
    /// Returns reference to vector of reactors.
    {
      return parallel_reactors_;
    }

    SocketReactor* reactor(std::size_t idx)
    /// Returns reference to the reactor at position idx.
    {
      return parallel_reactors_.at(idx).get();
    }

    std::size_t next()
    /// Returns the next reactor index.
    {
      return next_;
    }


  private:

    String name_;
    ServerSocket socket_;
    unsigned threads_;
    std::size_t    next_;
    Context & keeper_context_;
    Poco::Timespan timeout_;

    std::shared_ptr<SocketReactor> server_reactor_;
    ReactorVec     parallel_reactors_;
  };


}
