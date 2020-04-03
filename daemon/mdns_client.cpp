//
//  mdns_client.cpp
//
//  Copyright (c) 2019 2020 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "mdns_client.hpp"

#include <boost/asio.hpp>

#include "config.hpp"
#include "interface.hpp"
#include "log.hpp"
#include "rtsp_client.hpp"

#ifdef _USE_AVAHI_
void MDNSClient::resolve_callback(AvahiServiceResolver* r,
                                  AvahiIfIndex interface,
                                  AvahiProtocol protocol,
                                  AvahiResolverEvent event,
                                  const char* name,
                                  const char* type,
                                  const char* domain,
                                  const char* host_name,
                                  const AvahiAddress* address,
                                  uint16_t port,
                                  AvahiStringList* txt,
                                  AvahiLookupResultFlags flags,
                                  void* userdata) {
  MDNSClient& mdns = *(reinterpret_cast<MDNSClient*>(userdata));
  /* Called whenever a service has been resolved successfully or timed out */
  switch (event) {
    case AVAHI_RESOLVER_FAILURE:
      BOOST_LOG_TRIVIAL(error) << "avahi_client:: (Resolver) failed to resolve "
                               << "service " << name << " of type " << type
                               << " in domain " << domain << " : "
                               << avahi_strerror(avahi_client_errno(
                                      avahi_service_resolver_get_client(r)));
      break;

    case AVAHI_RESOLVER_FOUND:
      BOOST_LOG_TRIVIAL(debug) << "avahi_client:: (Resolver) "
                               << "service " << name << " of type " << type
                               << " in domain " << domain;

      char addr[AVAHI_ADDRESS_STR_MAX];
      avahi_address_snprint(addr, sizeof(addr), address);

      char info[256];
      snprintf(info, sizeof(info),
               "%s:%u (%s), "
               "local: %i, "
               "our_own: %i, "
               "wide_area: %i, "
               "multicast: %i, "
               "cached: %i",
               host_name, port, addr, !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
               !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
               !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
               !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
               !!(flags & AVAHI_LOOKUP_RESULT_CACHED));
      BOOST_LOG_TRIVIAL(debug) << "avahi_client:: (Resolver) " << info;

      boost::system::error_code ec;
      boost::asio::ip::address_v4::from_string(addr, ec);
      if (!ec) {
        /* if valid IPv4 address retrieve source data via RTSP */
        std::lock_guard<std::mutex> lock(mdns.sources_res_mutex_);

        /* have fun ;-) */
        mdns.sources_res_.emplace_back(std::async(
            std::launch::async,
            [&mdns, name_ = std::forward<std::string>(name),
             domain_ = std::forward<std::string>(domain),
             addr_ = std::forward<std::string>(addr),
             port_ = std::forward<std::string>(std::to_string(port))] {
              auto res = RTSPClient::describe(std::string("/by-name/") + name_,
                                              addr_, port_);
              if (res.first) {
                mdns.on_new_rtsp_source(name_, domain_, res.second);
              }
            }));
      }

      break;
  }

  avahi_service_resolver_free(r);
}

void MDNSClient::browse_callback(AvahiServiceBrowser* b,
                                 AvahiIfIndex interface,
                                 AvahiProtocol protocol,
                                 AvahiBrowserEvent event,
                                 const char* name,
                                 const char* type,
                                 const char* domain,
                                 AvahiLookupResultFlags flags,
                                 void* userdata) {
  MDNSClient& mdns = *(reinterpret_cast<MDNSClient*>(userdata));
  /* Called whenever a new services becomes available on the LAN or is removed
   * from the LAN */
  switch (event) {
    case AVAHI_BROWSER_FAILURE:
      BOOST_LOG_TRIVIAL(fatal) << "avahi_client:: (Browser) "
                               << avahi_strerror(avahi_client_errno(
                                      avahi_service_browser_get_client(b)));
      avahi_threaded_poll_quit(mdns.poll_.get());
      return;

    case AVAHI_BROWSER_NEW:
      BOOST_LOG_TRIVIAL(info) << "avahi_client:: (Browser) NEW: "
                              << "service " << name << " of type " << type
                              << " in domain " << domain;
      /* We ignore the returned resolver object. In the callback
         function we free it. If the server is terminated before
         the callback function is called the server will free
         the resolver for us. */
      if (!(avahi_service_resolver_new(mdns.client_.get(), interface, protocol,
                                       name, type, domain, AVAHI_PROTO_UNSPEC,
                                       AVAHI_LOOKUP_NO_TXT, resolve_callback,
                                       &mdns))) {
        BOOST_LOG_TRIVIAL(error)
            << "avahi_client:: "
            << "Failed to resolve service " << name << " : "
            << avahi_strerror(avahi_client_errno(mdns.client_.get()));
      }
      break;

    case AVAHI_BROWSER_REMOVE:
      BOOST_LOG_TRIVIAL(info) << "avahi_client:: (Browser) REMOVE: "
                              << "service " << name << " of type " << type
                              << " in domain " << domain;
      mdns.on_remove_rtsp_source(name, domain);
      break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
      BOOST_LOG_TRIVIAL(debug) << "avahi_client:: (Browser) ALL_FOR_NOW";
      break;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      BOOST_LOG_TRIVIAL(debug) << "avahi_client:: (Browser) CACHE_EXHAUSTED";
      break;
  }
}

void MDNSClient::client_callback(AvahiClient* client,
                                 AvahiClientState state,
                                 void* userdata) {
  MDNSClient& mdns = *(reinterpret_cast<MDNSClient*>(userdata));
  /* Called whenever the client or server state changes */

  switch (state) {
  case AVAHI_CLIENT_FAILURE:
    BOOST_LOG_TRIVIAL(fatal) << "avahi_client:: server connection failure: "
                             << avahi_strerror(avahi_client_errno(client));
    /* TODO reconnect if disconnected */
    avahi_threaded_poll_quit(mdns.poll_.get());
    break;

   case AVAHI_CLIENT_S_REGISTERING:
   case AVAHI_CLIENT_S_RUNNING:
   case AVAHI_CLIENT_S_COLLISION:
    /* Create the service browser */
    mdns.sb_.reset(avahi_service_browser_new(client,
                                        mdns.config_->get_interface_idx(),
                                        AVAHI_PROTO_INET, "_rtsp._tcp", nullptr,
                                        {}, browse_callback, &mdns));
    if (mdns.sb_ == nullptr) {
      BOOST_LOG_TRIVIAL(fatal)
          << "avahi_client:: failed to create service browser: "
          << avahi_strerror(avahi_client_errno(mdns.client_.get()));
      avahi_threaded_poll_quit(mdns.poll_.get());
    }
    break;

  case AVAHI_CLIENT_CONNECTING:
    break;
  }

}
#endif

bool MDNSClient::init() {
  if (running_) {
    return true;
  }

#ifdef _USE_AVAHI_
  /* allocate poll loop object */
  poll_.reset(avahi_threaded_poll_new());
  if (poll_ == nullptr) {
    BOOST_LOG_TRIVIAL(fatal)
        << "avahi_client:: failed to create threaded poll object";
    return false;
  }

  /* allocate a new client */
  int error;
  client_.reset(avahi_client_new(avahi_threaded_poll_get(poll_.get()),
                                 AVAHI_CLIENT_NO_FAIL, client_callback, this,
                                 &error));
  if (client_ == nullptr) {
    BOOST_LOG_TRIVIAL(fatal)
        << "avahi_client:: failed to create client: " << avahi_strerror(error);
    return false;
  }

  (void)avahi_threaded_poll_start(poll_.get());
#endif
  running_ = true;
  return true;
}

void MDNSClient::process_results() {
#ifdef _USE_AVAHI_
  std::lock_guard<std::mutex> lock(sources_res_mutex_);
  /* remove all completed results and populate remote sources list */
  sources_res_.remove_if([](auto& result) {
    if (!result.valid()) {
      /* if invalid future remove from the list */
      return true;
    }
    auto status = result.wait_for(std::chrono::milliseconds(0));
    if (status == std::future_status::ready) {
      result.get();
      /* if completed remove from the list */
      return true;
    }
    /* if not completed leave in the list */
    return false;
  });
#endif
}

bool MDNSClient::terminate() {
  if (running_) {
    running_ = false;
#ifdef _USE_AVAHI_
    /* remove all completed results and populate remote sources list */
    /* wait for all pending results and remove from list */
    std::lock_guard<std::mutex> lock(sources_res_mutex_);
    BOOST_LOG_TRIVIAL(fatal) << "avahi_client:: waiting for "
                             << sources_res_.size() << " RTSP clients";
    sources_res_.remove_if([](auto& result) {
      if (result.valid()) {
        result.wait();
      }
      return true;
    });

    avahi_threaded_poll_stop(poll_.get());
#endif
  }
  return true;
}
