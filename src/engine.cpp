#include <boost/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <msgpack.hpp>

#include "engine.hpp"
#include "schedulers.hpp"
#include "future.hpp"

using namespace yappi::core;
using namespace yappi::engine;
using namespace yappi::plugin;
using namespace yappi::persistance;
using namespace yappi::helpers;

engine_t::engine_t(zmq::context_t& context, registry_t& registry, storage_t& storage,
    const std::string& target
):
    m_context(context),
    m_registry(registry),
    m_storage(storage),
    m_target(target),
    m_default_thread_id(auto_uuid_t().get())
{
    syslog(LOG_DEBUG, "engine %s: starting", m_target.c_str());
}

engine_t::~engine_t() {
    syslog(LOG_DEBUG, "engine %s: terminating", m_target.c_str()); 
    m_threads.clear();
}

engine_t::thread_t::thread_t(zmq::context_t& context, std::auto_ptr<source_t> source,
    storage_t& storage, auto_uuid_t id
): 
    m_context(context),
    m_pipe(m_context, ZMQ_PUSH),
    m_source(source),
    m_storage(storage),
    m_id(id)
{
    syslog(LOG_DEBUG, "engine %s: starting thread %s", m_source->uri().c_str(), 
        m_id.get().c_str());

    m_pipe.bind("inproc://" + m_id.get());
    
    if(pthread_create(&m_thread, NULL, &bootstrap, this) == EAGAIN) {
        throw std::runtime_error("system thread limit exceeded");
    }
}

engine_t::thread_t::~thread_t() {
    syslog(LOG_DEBUG, "engine %s: terminating thread %s", m_source->uri().c_str(),
        m_id.get().c_str());
    
    Json::Value message;
    message["command"] = "terminate";
    
    send(message);
    pthread_join(m_thread, NULL); 
}

void* engine_t::thread_t::bootstrap(void* args) {
    thread_t* thread = static_cast<thread_t*>(args);

    overseer_t overseer(thread->m_context, *thread->m_source, thread->m_storage, thread->m_id);
    overseer.run();

    return NULL;
}

void engine_t::push(future_t* future, const Json::Value& args) {
    Json::Value message, response;
    std::string thread_id;

    if(!args.get("isolated", false).asBool()) {
        thread_id = m_default_thread_id;
    } else {
        thread_id = args.get("compartment", auto_uuid_t().get()).asString();
    }
    
    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it == m_threads.end()) {
        std::auto_ptr<source_t> source;
        std::auto_ptr<thread_t> thread;

        try {
            source.reset(m_registry.instantiate(m_target));
            thread.reset(new thread_t(m_context, source, m_storage, auto_uuid_t(thread_id)));
            boost::tie(it, boost::tuples::ignore) = m_threads.insert(thread_id, thread);
        } catch(const std::exception& e) {
            syslog(LOG_ERR, "engine %s: exception - %s", m_target.c_str(), e.what());
            response["error"] = e.what();
            future->fulfill(m_target, response);
            return;
        }
    }
        
    message["command"] = "start";
    message["future"] = future->serialize();
    message["args"] = args;
    
    it->second->send(message);
}

void engine_t::drop(future_t* future, const Json::Value& args) {
    Json::Value message, response;
    std::string thread_id;

    if(!args.get("isolated", false).asBool()) {
        thread_id = m_default_thread_id;
    } else {
        thread_id = args.get("compartment", "").asString();
    }

    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it != m_threads.end()) {
        message["command"] = "stop";
        message["future"] = future->serialize();
        message["args"] = args;
    
        it->second->send(message);
    } else {
        response["error"] = "thread not found";
        future->fulfill(m_target, response);
    }
}

void engine_t::reap(const std::string& thread_id) {
    thread_map_t::iterator it = m_threads.find(thread_id);

    if(it == m_threads.end()) {
        syslog(LOG_DEBUG, "engine %s: found an orphan - thread %s", 
            m_target.c_str(), thread_id.c_str());
        return;
    }

    m_threads.erase(it);
}

overseer_t::overseer_t(zmq::context_t& context, source_t& source, storage_t& storage,
    auto_uuid_t id
):
    m_context(context),
    m_pipe(m_context, ZMQ_PULL),
    m_futures(m_context, ZMQ_PUSH),
    m_reaper(m_context, ZMQ_PUSH),
    m_source(source),
    m_storage(storage),
    m_id(id),
    m_loop(),
    m_io(m_loop),
    m_suicide(m_loop),
    m_cleanup(m_loop),
    m_cached(false)
{
    // Connect to the engine's controlling socket
    // and set the socket watcher
    m_pipe.connect("inproc://" + m_id.get());
    m_io.set<overseer_t, &overseer_t::request>(this);
    m_io.start(m_pipe.fd(), EV_READ);

    // [CONFIG] Initializing suicide timer
    m_suicide.set<overseer_t, &overseer_t::timeout>(this);
    m_suicide.start(600.);
    
    // Cache cleanup watcher
    m_cleanup.set<overseer_t, &overseer_t::cleanup>(this);
    m_cleanup.start();

    // Connecting to the core's future sink
    m_futures.connect("inproc://futures");

    // Connecting to the core's reaper sink
    m_reaper.connect("inproc://reaper");

    // [CONFIG] Set timer compression threshold
    m_loop.set_timeout_collect_interval(0.500);

    // Signal a false event, in case the core 
    // has managed to send something already
    m_loop.feed_fd_event(m_pipe.fd(), EV_READ);
}

void overseer_t::run() {
    m_loop.loop();
}

void overseer_t::request(ev::io& w, int revents) {
    Json::Value result;
    std::string command, type;
    
    while(m_pipe.pending()) {
        Json::Value message;
        
        m_pipe.recv(message);
        command = message["command"].asString();
        type = message["args"].get("type", "auto").asString(); 
       
        if(command == "start") {
            if(type == "auto") {
                push<auto_scheduler_t>(message);
            } else if(type == "manual") {
                push<manual_scheduler_t>(message);
            } else if(type == "once") {
                once(message);
            } else {
                result["error"] = "invalid type";
                respond(message["future"], result);
            }
        } else if(command == "stop") {
            if(type == "auto") {
                drop<auto_scheduler_t>(message);
            } else if(type == "manual") {
                drop<manual_scheduler_t>(message);
            } else {
                result["error"] = "invalid type";
                respond(message["future"], result);
            }
        } else if(command == "terminate") {
            terminate();
            break;
        } else {
            result["error"] = "invalid command";
            respond(message["future"], result);
        }
    }
}
 
void overseer_t::timeout(ev::timer& w, int revents) {
    suicide();
}

void overseer_t::cleanup(ev::prepare& w, int revents) {
    m_cache.clear();
    m_cached = false;
}

dict_t overseer_t::fetch() {
    if(!m_cached) {
        try {
            m_cache = m_source.fetch();
            m_cached = true;
        } catch(const std::exception& e) {
            syslog(LOG_ERR, "engine %s: exception - %s", m_source.uri().c_str(), e.what());
            suicide();
        }
    }

    return m_cache;
}

template<class SchedulerType>
void overseer_t::push(const Json::Value& message) {
    Json::Value result;
    std::string scheduler_id;
    std::string token = message["future"]["token"].asString();
    std::string prefix;

    if(message["args"].get("isolated", false).asBool()) {
        prefix = m_id.get();
    };

    std::auto_ptr<SchedulerType> scheduler;

    try {
        scheduler.reset(new SchedulerType(m_source, message["args"]));
        scheduler_id = scheduler->id();
    } catch(const std::exception& e) {
        syslog(LOG_ERR, "engine %s: exception - %s", m_source.uri().c_str(), e.what());
        result["error"] = e.what();
        respond(message["future"], result);
        return;
    }
    
    // Scheduling
    if(m_slaves.find(scheduler_id) == m_slaves.end()) {
        scheduler->start(m_context, this);
        m_slaves.insert(scheduler_id, scheduler);

        if(m_suicide.is_active()) {
            syslog(LOG_DEBUG, "engine %s: suicide timer stopped", m_source.uri().c_str());
            m_suicide.stop();
        }
    }

    // ACL
    subscription_map_t::const_iterator begin, end;
    boost::tie(begin, end) = m_subscriptions.equal_range(token);
    subscription_map_t::value_type subscription = std::make_pair(token, scheduler_id);
    std::equal_to<subscription_map_t::value_type> equality;

    if(std::find_if(begin, end, boost::bind(equality, subscription, _1)) == end) {
        syslog(LOG_DEBUG, "engine %s: subscribing %s", m_source.uri().c_str(),
            token.c_str());
        m_subscriptions.insert(subscription);
    }
    
    // Persistance
    if(!message["args"].get("transient", false).asBool()) {
        std::string object_id = m_digest.get(prefix + scheduler_id + token);

        if(!m_storage.exists(object_id)) {
            Json::Value object;
            
            object["url"] = m_source.uri();
            object["args"] = message["args"];
            object["args"]["compartment"] = m_id.get();
            object["token"] = message["future"]["token"];
            
            m_storage.put(object_id, object);
        }
    }

    // Report to the core
    result["key"] = scheduler_id;
    result["compartment"] = m_id.get();
    respond(message["future"], result);
}

template<class SchedulerType>
void overseer_t::drop(const Json::Value& message) {
    Json::Value result;
    std::string token = message["future"]["token"].asString();
    std::string scheduler_id;

    std::auto_ptr<SchedulerType> scheduler;

    try {
        scheduler.reset(new SchedulerType(m_source, message["args"]));
        scheduler_id = scheduler->id();
    } catch(const std::exception& e) {
        syslog(LOG_ERR, "engine %s: exception - %s", m_source.uri().c_str(), e.what());
        result["error"] = e.what();
        respond(message["future"], result);
        return;
    }
   
    slave_map_t::iterator slave;
    subscription_map_t::iterator client;

    if((slave = m_slaves.find(scheduler_id)) != m_slaves.end()) {
        subscription_map_t::iterator begin, end;
        boost::tie(begin, end) = m_subscriptions.equal_range(token);
        subscription_map_t::value_type subscription = std::make_pair(token, scheduler_id);
        std::equal_to<subscription_map_t::value_type> equality;

        if((client = std::find_if(begin, end, boost::bind(equality, subscription, _1))) != end) {
            m_slaves.erase(slave);
            m_subscriptions.erase(client);
            
            if(m_slaves.empty()) {
                syslog(LOG_DEBUG, "engine %s: suicide timer started", m_source.uri().c_str());
                m_suicide.start(600.); // [CONFIG]
            }

            result["result"] = "success";
        } else {
            result["error"] = "not authorized";
        }
    } else {
        result["error"] = "scheduler not found";
    }

    respond(message["future"], result);
}

void overseer_t::once(const Json::Value& message) {
    Json::Value result;
    dict_t dict = fetch();

    for(dict_t::const_iterator it = dict.begin(); it != dict.end(); ++it) {
        result[it->first] = it->second;
    }

    // Report to the core
    respond(message["future"], result);

    // If it's a one-time isolated task, then it was a kamikaze mission,
    // and we can safely kill ourselves
    if(message["args"].get("isolated", false).asBool()) {
        suicide();
        return;
    }

    // Rearm the stall timer if it's active
    if(m_suicide.is_active()) {
        syslog(LOG_DEBUG, "engine %s: suicide timer rearmed", m_source.uri().c_str());
        m_suicide.stop();
        m_suicide.start(600.); // [CONFIG]
    }
}

void overseer_t::reap(const std::string& scheduler_id) {
    slave_map_t::iterator it = m_slaves.find(scheduler_id);

    if(it == m_slaves.end()) {
        syslog(LOG_ERR, "engine %s: found an orphan - scheduler %s", 
            m_source.uri().c_str(), scheduler_id.c_str());
        return;
    }

    m_slaves.erase(it);

    if(m_slaves.empty()) {
        syslog(LOG_DEBUG, "engine %s: suicide timer started", m_source.uri().c_str());
        m_suicide.start(600.); // [CONFIG]
    }
}

void overseer_t::terminate() {
    // Kill everything
    m_suicide.stop();
    m_io.stop();
    m_slaves.clear();
    m_cleanup.stop();
} 

void overseer_t::suicide() {
    Json::Value message;

    message["engine"] = m_source.uri();
    message["thread"] = m_id.get();

    // This is a suicide ;(
    m_reaper.send(message);    
}

scheduler_base_t::scheduler_base_t(source_t& source):
    m_source(source),
    m_stopping(false)
{}

scheduler_base_t::~scheduler_base_t() {
    if(m_watcher.get() && m_watcher->is_active()) {
        m_watcher->stop();
    }
}

void scheduler_base_t::start(zmq::context_t& context, overseer_t* overseer) {
    m_overseer = overseer;
    
    m_uplink.reset(new net::blob_socket_t(context, ZMQ_PUSH));
    m_uplink->connect("inproc://events");
    
    m_watcher.reset(new ev::periodic(m_overseer->binding()));
    m_watcher->set<scheduler_base_t, &scheduler_base_t::publish>(this);
    ev_periodic_set(static_cast<ev_periodic*>(m_watcher.get()), 0, 0, thunk);
    m_watcher->start();
}

void scheduler_base_t::publish(ev::periodic& w, int revents) {
    if(m_stopping) {
        m_watcher->stop();
        m_overseer->reap(id());
        return;
    }
    
    dict_t dict = m_overseer->fetch();

    // Do nothing if plugin has returned an empty dict
    if(dict.size() == 0) {
        return;
    }

    zmq::message_t message(m_id.length());
    memcpy(message.data(), m_id.data(), m_id.length());
    m_uplink->send(message, ZMQ_SNDMORE);

    // Serialize the dict
    msgpack::sbuffer buffer;
    msgpack::pack(buffer, dict);

    // And send it
    message.rebuild(buffer.size());
    memcpy(message.data(), buffer.data(), buffer.size());
    m_uplink->send(message);
}

ev::tstamp scheduler_base_t::thunk(ev_periodic* w, ev::tstamp now) {
    scheduler_base_t* scheduler = static_cast<scheduler_base_t*>(w->data);

    try {
        return scheduler->reschedule(now);
    } catch(const std::exception& e) {
        syslog(LOG_ERR, "engine: %s scheduler is broken - %s",
            scheduler->id().c_str(), e.what());
        scheduler->stop();
        return now;
    }
}
