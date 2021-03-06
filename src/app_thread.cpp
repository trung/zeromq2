/*
    Copyright (c) 2007-2010 iMatix Corporation

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <new>
#include <algorithm>

#include "../include/zmq.h"

#include "platform.hpp"

#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#if defined _MSC_VER
#include <intrin.h>
#endif
#else
#include <unistd.h>
#endif

#include "app_thread.hpp"
#include "dispatcher.hpp"
#include "fd_signaler.hpp"
#include "ypollset.hpp"
#include "err.hpp"
#include "pipe.hpp"
#include "config.hpp"
#include "socket_base.hpp"
#include "p2p.hpp"
#include "pub.hpp"
#include "sub.hpp"
#include "req.hpp"
#include "rep.hpp"
#include "xreq.hpp"
#include "xrep.hpp"
#include "upstream.hpp"
#include "downstream.hpp"

//  If the RDTSC is available we use it to prevent excessive
//  polling for commands. The nice thing here is that it will work on any
//  system with x86 architecture and gcc or MSVC compiler.
#if (defined __GNUC__ && (defined __i386__ || defined __x86_64__)) ||\
    (defined _MSC_VER && (defined _M_IX86 || defined _M_X64))
#define ZMQ_DELAY_COMMANDS
#endif

zmq::app_thread_t::app_thread_t (dispatcher_t *dispatcher_, int thread_slot_,
      int flags_) :
    object_t (dispatcher_, thread_slot_),
    last_processing_time (0)
{
    if (flags_ & ZMQ_POLL) {
        signaler = new (std::nothrow) fd_signaler_t;
        zmq_assert (signaler);
    }
    else {
        signaler = new (std::nothrow) ypollset_t;
        zmq_assert (signaler);
    }
}

zmq::app_thread_t::~app_thread_t ()
{
    zmq_assert (sockets.empty ());
    zmq_assert (signaler);
    delete signaler;
}

zmq::i_signaler *zmq::app_thread_t::get_signaler ()
{
    return signaler;
}

void zmq::app_thread_t::process_commands (bool block_, bool throttle_)
{
    uint64_t signals;
    if (block_)
        signals = signaler->poll ();
    else {

#if defined ZMQ_DELAY_COMMANDS
        //  Optimised version of command processing - it doesn't have to check
        //  for incoming commands each time. It does so only if certain time
        //  elapsed since last command processing. Command delay varies
        //  depending on CPU speed: It's ~1ms on 3GHz CPU, ~2ms on 1.5GHz CPU
        //  etc. The optimisation makes sense only on platforms where getting
        //  a timestamp is a very cheap operation (tens of nanoseconds).
        if (throttle_) {

            //  Get timestamp counter.
#if defined __GNUC__
            uint32_t low;
            uint32_t high;
            __asm__ volatile ("rdtsc" : "=a" (low), "=d" (high));
            uint64_t current_time = (uint64_t) high << 32 | low;
#elif defined _MSC_VER
            uint64_t current_time = __rdtsc ();
#else
#error
#endif

            //  Check whether certain time have elapsed since last command
            //  processing.
            if (current_time - last_processing_time <= max_command_delay)
                return;
            last_processing_time = current_time;
        }
#endif

        //  Check whether there are any commands pending for this thread.
        signals = signaler->check ();
    }

    if (signals) {

        //  Traverse all the possible sources of commands and process
        //  all the commands from all of them.
        for (int i = 0; i != thread_slot_count (); i++) {
            if (signals & (uint64_t (1) << i)) {
                command_t cmd;
                while (dispatcher->read (i, get_thread_slot (), &cmd))
                    cmd.destination->process_command (cmd);
            }
        }
    }
}

zmq::socket_base_t *zmq::app_thread_t::create_socket (int type_)
{
    socket_base_t *s = NULL;
    switch (type_) {
    case ZMQ_P2P:
        s = new (std::nothrow) p2p_t (this);
        break;
    case ZMQ_PUB:
        s = new (std::nothrow) pub_t (this);
        break;
    case ZMQ_SUB:
        s = new (std::nothrow) sub_t (this);
        break;
    case ZMQ_REQ:
        s = new (std::nothrow) req_t (this);
        break;
    case ZMQ_REP:
        s = new (std::nothrow) rep_t (this);
        break;
    case ZMQ_XREQ:
        s = new (std::nothrow) xreq_t (this);
        break;
    case ZMQ_XREP:
        s = new (std::nothrow) xrep_t (this);
        break;       
    case ZMQ_UPSTREAM:
        s = new (std::nothrow) upstream_t (this);
        break;
    case ZMQ_DOWNSTREAM:
        s = new (std::nothrow) downstream_t (this);
        break;
    default:
        if (sockets.empty ())
            dispatcher->no_sockets (this);
        errno = EINVAL;
        return NULL;
    }
    zmq_assert (s);

    sockets.push_back (s);

    return s;
}

void zmq::app_thread_t::remove_socket (socket_base_t *socket_)
{
    sockets.erase (socket_);
    if (sockets.empty ())
        dispatcher->no_sockets (this);
}
