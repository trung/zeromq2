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

#include "../include/zmq.h"

#include "rep.hpp"
#include "err.hpp"
#include "pipe.hpp"

zmq::rep_t::rep_t (class app_thread_t *parent_) :
    socket_base_t (parent_),
    active (0),
    current (0),
    sending_reply (false),
    more (false),
    reply_pipe (NULL)
{
    options.requires_in = true;
    options.requires_out = true;

    //  We don't need immediate connect. We'll be able to send messages
    //  (replies) only when connection is established and thus requests
    //  can arrive anyway.
    options.immediate_connect = false;
}

zmq::rep_t::~rep_t ()
{
}

void zmq::rep_t::xattach_pipes (class reader_t *inpipe_,
    class writer_t *outpipe_, const blob_t &peer_identity_)
{
    zmq_assert (inpipe_ && outpipe_);
    zmq_assert (in_pipes.size () == out_pipes.size ());

    in_pipes.push_back (inpipe_);
    in_pipes.swap (active, in_pipes.size () - 1);
    out_pipes.push_back (outpipe_);
    out_pipes.swap (active, out_pipes.size () - 1);
    active++;
}

void zmq::rep_t::xdetach_inpipe (class reader_t *pipe_)
{
    zmq_assert (sending_reply || !more || in_pipes [current] != pipe_);

    zmq_assert (pipe_);
    zmq_assert (in_pipes.size () == out_pipes.size ());

    in_pipes_t::size_type index = in_pipes.index (pipe_);

    //  If corresponding outpipe is still in place simply nullify the pointer
    //  to the inpipe and move it to the passive state.
    if (out_pipes [index]) {
        in_pipes [index] = NULL;
        if (in_pipes.index (pipe_) < active) {
            active--;
            in_pipes.swap (index, active);
            out_pipes.swap (index, active);
            if (current == active)
                current = 0;
        }
        return;
    }

    //  Now both inpipe and outpipe are detached. Remove them from the lists.
    if (index < active) {
        active--;
        if (current == active)
            current = 0;
    }

    in_pipes.erase (index);
    out_pipes.erase (index);
}

void zmq::rep_t::xdetach_outpipe (class writer_t *pipe_)
{
    zmq_assert (!sending_reply || !more || reply_pipe != pipe_);

    zmq_assert (pipe_);
    zmq_assert (in_pipes.size () == out_pipes.size ());

    out_pipes_t::size_type index = out_pipes.index (pipe_);

    //  If the connection we've got the request from disconnects,
    //  there's nowhere to send the reply. Forget about the reply pipe.
    //  Once the reply is sent it will be dropped.
    if (sending_reply && pipe_ == reply_pipe)
        reply_pipe = NULL;

    //  If corresponding inpipe is still in place simply nullify the pointer
    //  to the outpipe.
    if (in_pipes [index]) {
        out_pipes [index] = NULL;
        if (out_pipes.index (pipe_) < active) {
            active--;
            in_pipes.swap (index, active);
            out_pipes.swap (index, active);
            if (current == active)
                current = 0;
        }
        return;
    }

    //  Now both inpipe and outpipe are detached. Remove them from the lists.
    if (out_pipes.index (pipe_) < active) {
        active--;
        if (current == active)
            current = 0;
    }

    in_pipes.erase (index);
    out_pipes.erase (index);
}

void zmq::rep_t::xkill (class reader_t *pipe_)
{
    //  Move the pipe to the list of inactive pipes.
    in_pipes_t::size_type index = in_pipes.index (pipe_);
    active--;
    in_pipes.swap (index, active);
    out_pipes.swap (index, active);
}

void zmq::rep_t::xrevive (class reader_t *pipe_)
{
    //  Move the pipe to the list of active pipes.
    in_pipes_t::size_type index = in_pipes.index (pipe_);
    in_pipes.swap (index, active);
    out_pipes.swap (index, active);
    active++;
}

void zmq::rep_t::xrevive (class writer_t *pipe_)
{
}

int zmq::rep_t::xsetsockopt (int option_, const void *optval_,
    size_t optvallen_)
{
    errno = EINVAL;
    return -1;
}

int zmq::rep_t::xsend (zmq_msg_t *msg_, int flags_)
{
    if (!sending_reply) {
        errno = EFSM;
        return -1;
    }

    if (reply_pipe) {

        //  Push message to the reply pipe.
        bool written = reply_pipe->write (msg_);
        zmq_assert (!more || written);

        //  The pipe is full...
        //  When this happens, we simply return an error.
        //  This makes REP sockets vulnerable to DoS attack when
        //  misbehaving requesters stop collecting replies.
        //  TODO: Tear down the underlying connection (?)
        if (!written) {
            errno = EAGAIN;
            return -1;
        }
    }
    else {

        //  If the requester have disconnected in the meantime, drop the reply.
        zmq_msg_close (msg_);
    }

    //  Check whether it's last part of the reply.
    more = msg_->flags & ZMQ_MSG_MORE;

    //  Flush the reply to the requester.
    if (!more) {
        reply_pipe->flush ();
        sending_reply = false;
        reply_pipe = NULL;
    }

    //  Detach the message from the data buffer.
    int rc = zmq_msg_init (msg_);
    zmq_assert (rc == 0);

    return 0;
}

int zmq::rep_t::xrecv (zmq_msg_t *msg_, int flags_)
{
    //  Deallocate old content of the message.
    zmq_msg_close (msg_);

    if (sending_reply) {
        errno = EFSM;
        return -1;
    }

    //  Round-robin over the pipes to get next message.
    for (int count = active; count != 0; count--) {
        bool fetched = in_pipes [current]->read (msg_);
        zmq_assert (!(more && !fetched));
        
        if (fetched) {
            more = msg_->flags & ZMQ_MSG_MORE;
            if (!more) {
                reply_pipe = out_pipes [current];
                sending_reply = true;
                current++;
                if (current >= active)
                    current = 0;
            }
            return 0;
        }
    }

    //  No message is available. Initialise the output parameter
    //  to be a 0-byte message.
    zmq_msg_init (msg_);
    errno = EAGAIN;
    return -1;
}

bool zmq::rep_t::xhas_in ()
{
    if (!sending_reply && more)
        return true;

    for (int count = active; count != 0; count--) {
        if (in_pipes [current]->check_read ())
            return !sending_reply;
        current++;
        if (current >= active)
            current = 0;
    }

    return false;
}

bool zmq::rep_t::xhas_out ()
{
    if (sending_reply && more)
        return true;

    //  TODO: No check for write here...
    return sending_reply;
}

