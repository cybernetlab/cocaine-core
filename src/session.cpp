/*
    Copyright (c) 2011-2014 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2014 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/rpc/session.hpp"

#include "cocaine/rpc/dispatch.hpp"
#include "cocaine/rpc/upstream.hpp"

using namespace cocaine;
using namespace cocaine::io;

class session_t::channel_t {
    std::shared_ptr<basic_dispatch_t> dispatch;
    std::shared_ptr<basic_upstream_t> upstream;

public:
    channel_t(const std::shared_ptr<basic_dispatch_t>& dispatch_, const std::shared_ptr<basic_upstream_t>& upstream_):
        dispatch(dispatch_),
        upstream(upstream_)
    { }

    void
    invoke(const message_t& message) {
        if(!dispatch) {
            // TODO: COCAINE-82 adds a 'client' error category.
            throw cocaine::error_t("dispatch has been deactivated");
        }

        if((dispatch = dispatch->invoke(message, upstream)) == nullptr) {
            // NOTE: If the client has sent us the last message according to the dispatch graph, then
            // revoke the channel.
            upstream->revoke();
        }
    }
};

session_t::session_t(std::unique_ptr<io::channel<io::socket<io::tcp>>>&& ptr_, const std::shared_ptr<io::basic_dispatch_t>& prototype_):
    ptr(std::move(ptr_)),
    prototype(prototype_),
    max_channel(0)
{ }

void
session_t::invoke(const message_t& message) {
    channel_map_t::const_iterator lb, ub;
    channel_map_t::key_type index = message.band();

    std::shared_ptr<channel_t> channel;

    {
        auto locked = channels.synchronize();

        std::tie(lb, ub) = locked->equal_range(index);

        if(lb == ub) {
            if(!prototype || index <= max_channel) {
                return;
            }

            // NOTE: Checking whether channel number is always higher than the previous channel number
            // is similar to an infinite TIME_WAIT timeout for TCP sockets. It might be not the best
            // aproach, but since we have 2^64 possible channels, unlike 2^16 ports for sockets, it is
            // fit to avoid stray messages.

            max_channel = index;

            std::tie(lb, std::ignore) = locked->insert({index, std::make_shared<channel_t>(
                prototype,
                std::make_shared<basic_upstream_t>(shared_from_this(), index)
            )});
        }

        // NOTE: The virtual channel pointer is copied here so that if the slot decides to close the
        // virtual channel, it won't destroy it inside the channel_t::invoke(). Instead, it will be
        // destroyed when this function scope is exited, liberating us from thinking of some voodoo
        // magic to handle it.

        channel = lb->second;
    }

    channel->invoke(message);
}

std::shared_ptr<basic_upstream_t>
session_t::invoke(const std::shared_ptr<io::basic_dispatch_t>& dispatch) {
    auto locked = channels.synchronize();

    auto index = ++max_channel;
    auto upstream = std::make_shared<basic_upstream_t>(shared_from_this(), index);

    // TODO: Think about skipping dispatch registration in case of fire-and-forget service events.
    locked->insert({index, std::make_shared<channel_t>(dispatch, upstream)});

    return upstream;
}

void
session_t::detach() {
    std::lock_guard<std::mutex> guard(mutex);

    // NOTE: This invalidates and closes the internal connection pointer and destroys the protocol
    // dispatches, but the session itself might still be accessible via upstreams in other threads.
    // And that's okay, since it has no resources associated with it anymore.

    channels->clear();
    ptr.reset();
}

void
session_t::revoke(uint64_t index) {
    channels->erase(index);
}
