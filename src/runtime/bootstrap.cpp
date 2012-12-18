/*
    Copyright (c) 2011-2012 Andrey Sibiryov <me@kobology.ru>
    Copyright (c) 2011-2012 Other contributors as noted in the AUTHORS file.

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

#include "cocaine/runtime/pid_file.hpp"

#include "cocaine/config.hpp"
#include "cocaine/asio.hpp"
#include "cocaine/context.hpp"
#include "cocaine/logging.hpp"

#include "cocaine/api/service.hpp"

#include "cocaine/helpers/format.hpp"

#include <iostream>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

using namespace cocaine;
using namespace cocaine::logging;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

namespace {
    struct runtime_t {
        runtime_t(context_t& context,
                  const std::vector<std::string>& services):
            m_context(context),
            m_log(new log_t(context, "runtime"))
        {
            for(auto&& service: services) {
                COCAINE_LOG_INFO(m_log, "starting the '%s' service", service);

                m_services.emplace(
                    service,
                    api::service(m_context, service)
                );
            }

            // Signal handling

            m_sigint.set<runtime_t, &runtime_t::on_terminate>(this);
            m_sigint.start(SIGINT);

            m_sigterm.set<runtime_t, &runtime_t::on_terminate>(this);
            m_sigterm.start(SIGTERM);

            m_sigquit.set<runtime_t, &runtime_t::on_terminate>(this);
            m_sigquit.start(SIGQUIT);
        }

        ~runtime_t() {
            for(auto&& service: m_services) {
                service.second->terminate();
            }

            m_services.clear();
        }

        void
        run() {
            m_loop.loop();
        }

    private:
        void
        on_terminate(ev::sig&, int) {
            m_loop.unloop(ev::ALL);
        }

    private:
        context_t& m_context;
        std::unique_ptr<log_t> m_log;

        // Main event loop, able to handle signals.
        ev::default_loop m_loop;

        // Signal watchers.
        ev::sig m_sigint;
        ev::sig m_sigterm;
        ev::sig m_sigquit; 

#if BOOST_VERSION >= 103600
        typedef boost::unordered_map<
#else
        typedef std::map<
#endif
            std::string,
            std::unique_ptr<api::service_t>
        > service_map_t;

        // Services.
        service_map_t m_services;
    };
}

int main(int argc, char * argv[]) {
    po::options_description general_options("General options"),
                            service_options("Service options"),
                            combined_options;
    
    po::variables_map vm;

    general_options.add_options()
        ("help,h", "show this message")
        ("version,v", "show version and build information")
        ("configuration,c", po::value<std::string>(), "location of the configuration file")
        ("daemonize,d", "daemonize on start")
        ("pidfile,p", po::value<std::string>(), "location of a pid file");

    service_options.add_options()
        ("service,s", po::value<std::vector<std::string>>(), "names of the services");
    
    combined_options.add(general_options)
                    .add(service_options);

    try {
        po::store(
            po::command_line_parser(argc, argv).
                options(combined_options).
                run(),
            vm);
        po::notify(vm);
    } catch(const po::unknown_option& e) {
        std::cerr << cocaine::format("Error: %s.", e.what()) << std::endl;
        return EXIT_FAILURE;
    } catch(const po::ambiguous_option& e) {
        std::cerr << cocaine::format("Error: %s.", e.what()) << std::endl;
        return EXIT_FAILURE;
    }

    if(vm.count("help")) {
        std::cout << cocaine::format("Usage: %s <service-name> [options]", argv[0]) << std::endl;
        std::cout << combined_options;
        return EXIT_SUCCESS;
    }

    if(vm.count("version")) {
        std::cout << cocaine::format("Cocaine %d", COCAINE_VERSION) << std::endl;
        return EXIT_SUCCESS;
    }

    // Validation

    if(!vm.count("configuration")) {
        std::cerr << "Error: no configuration file location has been specified." << std::endl;
        return EXIT_FAILURE;
    }

    if(!vm.count("service")) {
        std::cerr << "Error: no services has been specified." << std::endl;
        return EXIT_FAILURE;
    }

    // Startup

    std::unique_ptr<config_t> config;

    try {
        config.reset(new config_t(vm["configuration"].as<std::string>()));
    } catch(const std::exception& e) {
        std::cerr << cocaine::format("Error: unable to initialize the configuration - %s.", e.what()) << std::endl;
        return EXIT_FAILURE;
    }

    std::unique_ptr<pid_file_t> pidfile;

    if(vm.count("daemonize")) {
        fs::path pid_path;

        if(!vm["pidfile"].empty()) {
            pid_path = fs::path(vm["pidfile"].as<std::string>());
        } else {
            pid_path = cocaine::format("%s/cocained.pid", config->path.runtime);
        }

        if(daemon(0, 0) < 0) {
            std::cerr << "Error: daemonization failed." << std::endl;
            return EXIT_FAILURE;
        }

        try {
            pidfile.reset(new pid_file_t(pid_path));
        } catch(const std::exception& e) {
            std::cerr << cocaine::format("Error: %s.", e.what()) << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::unique_ptr<context_t> context;

    try {
        context.reset(new context_t(*config, "logger/core"));
    } catch(const std::exception& e) {
        std::cerr << cocaine::format("Error: unable to initialize the context - %s.", e.what()) << std::endl;
        return EXIT_FAILURE;
    }

    std::unique_ptr<runtime_t> runtime;

    try {
        runtime.reset(new runtime_t(
            *context,
            vm["service"].as<std::vector<std::string>>()
        ));
    } catch(const std::exception& e) {
        std::cerr << cocaine::format("Error: unable to initialize the runtime - %s.", e.what()) << std::endl;
        return EXIT_FAILURE;
    }

    runtime->run();

    return EXIT_SUCCESS;
}