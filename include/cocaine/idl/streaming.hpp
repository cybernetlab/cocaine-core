/*
    Copyright (c) 2011-2013 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

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

#ifndef COCAINE_STREAMING_SERVICE_INTERFACE_HPP
#define COCAINE_STREAMING_SERVICE_INTERFACE_HPP

#include "cocaine/rpc/protocol.hpp"

namespace cocaine { namespace io {

// Streaming service interface

template<class T>
struct streaming {

struct chunk {
    typedef streaming_tag<T> tag;
    typedef recursive_tag    transition_type;

    static
    const char*
    alias() {
        return "write";
    }

    typedef typename std::conditional<
        boost::mpl::is_sequence<T>::value,
        T,
        boost::mpl::list<T>
    >::type tuple_type;
};

struct error {
    typedef streaming_tag<T> tag;

    static
    const char*
    alias() {
        return "error";
    }

    typedef boost::mpl::list<
     /* Error code. */
        int,
     /* Human-readable error description. */
        std::string
    > tuple_type;
};

struct choke {
    typedef streaming_tag<T> tag;

    static
    const char*
    alias() {
        return "close";
    }
};

}; // struct streaming

template<class T>
struct protocol<streaming_tag<T>> {
    typedef boost::mpl::int_<
        1
    >::type version;

    typedef typename boost::mpl::list<
        typename streaming<T>::chunk,
        typename streaming<T>::error,
        typename streaming<T>::choke
    >::type messages;

    typedef streaming<T> type;
};

}} // namespace cocaine::io

#endif
