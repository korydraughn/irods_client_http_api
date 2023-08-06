#include "common.hpp"
#include "globals.hpp"
#include "handlers.hpp"
#include "log.hpp"
#include "session.hpp"
#include "version.hpp"

#include <irods/connection_pool.hpp>
#include <irods/irods_configuration_keywords.hpp>
#include <irods/process_stash.hpp>
#include <irods/rcConnect.h>
#include <irods/rcMisc.h>
#include <irods/rodsClient.h>

//#include <curl/curl.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
//#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/config.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/program_options.hpp>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// clang-format off
namespace beast = boost::beast; // from <boost/beast.hpp>
//namespace http  = beast::http;  // from <boost/beast/http.hpp>
namespace net   = boost::asio;  // from <boost/asio.hpp>
namespace po    = boost::program_options;
namespace log   = irods::http::log;

using json = nlohmann::json;
using tcp  = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>

// IRODS_HTTP_API_BASE_URL is a macro defined by the CMakeLists.txt.
const irods::http::request_handler_map_type req_handlers{
    {IRODS_HTTP_API_BASE_URL "/authenticate", irods::http::handler::authentication},
    {IRODS_HTTP_API_BASE_URL "/collections",  irods::http::handler::collections},
//  {IRODS_HTTP_API_BASE_URL "/config",       irods::http::handler::configuration},
    {IRODS_HTTP_API_BASE_URL "/data-objects", irods::http::handler::data_objects},
    {IRODS_HTTP_API_BASE_URL "/info",         irods::http::handler::information},
    {IRODS_HTTP_API_BASE_URL "/metadata",     irods::http::handler::metadata},
    {IRODS_HTTP_API_BASE_URL "/query",        irods::http::handler::query},
    {IRODS_HTTP_API_BASE_URL "/resources",    irods::http::handler::resources},
    {IRODS_HTTP_API_BASE_URL "/rules",        irods::http::handler::rules},
    {IRODS_HTTP_API_BASE_URL "/tickets",      irods::http::handler::tickets},
    {IRODS_HTTP_API_BASE_URL "/users-groups", irods::http::handler::users_groups},
    {IRODS_HTTP_API_BASE_URL "/zones",        irods::http::handler::zones}
};
// clang-format on

// Accepts incoming connections and launches the sessions.
class listener : public std::enable_shared_from_this<listener>
{
  public:
    listener(net::io_context& ioc, const tcp::endpoint& endpoint, const json& _config)
        : ioc_{ioc}
        , acceptor_{net::make_strand(ioc)}
        , max_rbuffer_size_{_config.at(json::json_pointer{"/http_server/requests/max_rbuffer_size_in_bytes"}).get<int>()}
        , timeout_in_secs_{_config.at(json::json_pointer{"/http_server/requests/timeout_in_seconds"}).get<int>()}
    {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            irods::fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            irods::fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec) {
            irods::fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            irods::fail(ec, "listen");
            return;
        }
    } // listener (constructor)

    // Start accepting incoming connections.
    auto run() -> void
    {
        do_accept();
    } // run

  private:
    auto do_accept() -> void
    {
        // The new connection gets its own strand
        acceptor_.async_accept(
            net::make_strand(ioc_),
            beast::bind_front_handler(
                &listener::on_accept,
                shared_from_this()));
    } // do_accept

    auto on_accept(beast::error_code ec, tcp::socket socket) -> void
    {
        if (ec) {
            irods::fail(ec, "accept");
            //return; // To avoid infinite loop
        }
        else {
            // Create the session and run it
            std::make_shared<irods::http::session>(
                std::move(socket), req_handlers, max_rbuffer_size_, timeout_in_secs_)->run();
        }

        // Accept another connection
        do_accept();
    } // on_accept

    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    const int max_rbuffer_size_;
    const int timeout_in_secs_;
}; // class listener

auto print_version_info() -> void
{
    namespace version = irods::http::version;
    const std::string_view sha = version::sha;
    constexpr auto sha_size = 7;
    fmt::print("{} v{}-{}\n", version::binary_name, version::api_version, sha.substr(0, sha_size));
} // print_version_info

auto print_configuration_template() -> void
{
    fmt::print(R"({{
    "http_server": {{
        "host": "0.0.0.0",
        "port": 9000,

        "log_level": "warn",

        "authentication": {{
            "eviction_check_interval_in_seconds": 60,

            "basic": {{
                "timeout_in_seconds": 3600
            }}
        }},

        "requests": {{
            "threads": 3,
            "max_rbuffer_size_in_bytes": 8388608,
            "timeout_in_seconds": 30
        }},

        "background_io": {{
            "threads": 3
        }}
    }},

    "irods_client": {{
        "host": "<string>",
        "port": 1247,
        "zone": "<string>",

        "tls": {{
            "client_server_policy": "<string>",
            "ca_certificate_file": "<string>",
            "certificate_chain_file": "<string>",
            "dh_params_file": "<string>",
            "verify_server": "<string>"
        }},

        "proxy_rodsadmin": {{
            "username": "<string>",
            "password": "<string>"
        }},

        "connection_pool": {{
            "size": 6,
            "refresh_timeout_in_seconds": 600
        }},

        "max_rbuffer_size_in_bytes": 8192,
        "max_wbuffer_size_in_bytes": 8192,

        "max_number_of_rows_per_catalog_query": 15
    }}
}}
)");
} // print_configuration_template

auto print_usage() -> void
{
    fmt::print(R"_(irods_http_api - Exposes the iRODS API over HTTP

Usage: irods_http_api [OPTION]... CONFIG_FILE_PATH

CONFIG_FILE_PATH must point to a file containing a JSON structure containing
configuration options.

--dump-config-template can be used to generate a default configuration file.
See this option's description for more information.

Options:
      --dump-config-template
                     Print configuration template to stdout and exit. Some
                     options have values which act as placeholders. If used
                     to generate a configuration file, those options will
                     need to be updated.
  -h, --help         Display this help message and exit.
  -v, --version      Display version information and exit.

)_");

    print_version_info();
} // print_usage

auto set_log_level(const json& _config) -> void
{
    const auto iter = _config.find("log_level");

    if (iter == std::end(_config)) {
        spdlog::set_level(spdlog::level::info);
    }

    const auto& lvl_string = iter->get_ref<const std::string&>();
    auto lvl_enum = spdlog::level::info;

    // clang-format off
    if      (lvl_string == "trace")    { lvl_enum = spdlog::level::trace; }
    else if (lvl_string == "info")     { lvl_enum = spdlog::level::info; }
    else if (lvl_string == "debug")    { lvl_enum = spdlog::level::debug; }
    else if (lvl_string == "warn")     { lvl_enum = spdlog::level::warn; }
    else if (lvl_string == "error")    { lvl_enum = spdlog::level::err; }
    else if (lvl_string == "critical") { lvl_enum = spdlog::level::critical; }
    else                               { log::warn("Invalid log_level. Setting to [info]."); }
    // clang-format on

    spdlog::set_level(lvl_enum);
} // set_log_level

auto init_tls(const json& _config) -> void
{
    const auto set_env_var = [&](const auto& _json_ptr_path, const char* _env_var, const char* _default_value = "")
    {
        using json_ptr = json::json_pointer;

        if (const auto v = _config.value(json_ptr{_json_ptr_path}, _default_value); !v.empty()) {
            const auto env_var_upper = boost::to_upper_copy<std::string>(_env_var);
            log::trace("Setting environment variable [{}] to [{}].", env_var_upper, v);
            setenv(env_var_upper.c_str(), v.c_str(), 1); // NOLINT(concurrency-mt-unsafe)
        }
    };

    set_env_var("/irods_client/tls/client_server_policy", irods::KW_CFG_IRODS_CLIENT_SERVER_POLICY, "CS_NEG_REFUSE");
    set_env_var("/irods_client/tls/ca_certificate_file", irods::KW_CFG_IRODS_SSL_CA_CERTIFICATE_FILE);
    set_env_var("/irods_client/tls/certificate_chain_file", irods::KW_CFG_IRODS_SSL_CERTIFICATE_CHAIN_FILE);
    set_env_var("/irods_client/tls/dh_params_file", irods::KW_CFG_IRODS_SSL_DH_PARAMS_FILE);
    set_env_var("/irods_client/tls/verify_server", irods::KW_CFG_IRODS_SSL_VERIFY_SERVER, "cert");
} // init_tls

auto init_irods_connection_pool(const json& _config) -> irods::connection_pool
{
    const auto& client = _config.at("irods_client");
    const auto& zone = client.at("zone").get_ref<const std::string&>();
    const auto& conn_pool = client.at("connection_pool");
    const auto& rodsadmin = client.at("rodsadmin");
    const auto& username = rodsadmin.at("username").get_ref<const std::string&>();

    return {
        conn_pool.at("size").get<int>(),
        client.at("host").get_ref<const std::string&>(),
        client.at("port").get<int>(),
        {{username, zone}},
        {username, zone},
        conn_pool.at("refresh_timeout_in_seconds").get<int>(),
        [pw = rodsadmin.at("password").get<std::string>()](RcComm& _comm) mutable {
            if (const auto ec = clientLoginWithPassword(&_comm, pw.data()); ec != 0) {
                throw std::invalid_argument{fmt::format("Could not authenticate rodsadmin user: [{}]", ec)};
            }
        }
    };
} // init_irods_connection_pool

class bearer_token_eviction_manager
{
    net::steady_timer timer_;
    std::chrono::seconds interval_;

  public:
    bearer_token_eviction_manager(net::io_context& _io, std::chrono::seconds _eviction_check_interval)
        : timer_{_io}
        , interval_{_eviction_check_interval}
    {
        evict();
    } // constructor

  private:
    auto evict() -> void
    {
        timer_.expires_after(interval_);
        timer_.async_wait([this](const auto& _ec) {
            if (_ec) {
                return;
            }

            log::trace("Evicting expired bearer tokens ...");
            irods::process_stash::erase_if([](const auto& _k, const auto& _v) {
                try {
                    const auto* p = boost::any_cast<const irods::http::authenticated_client_info>(&_v);
                    const auto erase_token = (p && std::chrono::steady_clock::now() >= p->expires_at);

                    if (erase_token) {
                        log::debug("Evicted bearer token [{}].", _k);
                    }

                    return erase_token;
                }
                catch (...) {
                }

                return false;
            });

            evict();
        });
    } // evict
}; // class bearer_token_eviction_manager

auto main(int _argc, char* _argv[]) -> int
{
    po::options_description opts_desc{""};
    opts_desc.add_options()
        ("config-file,f", po::value<std::string>(), "")
        ("dump-config-template", "")
        ("help,h", "")
        ("version,v", "");

    po::positional_options_description pod;
    pod.add("config-file", 1);

    set_ips_display_name("irods_http_api");

    try {
        po::variables_map vm;
        po::store(po::command_line_parser(_argc, _argv).options(opts_desc).positional(pod).run(), vm);
        po::notify(vm);

        if (vm.count("help") > 0) {
            print_usage();
            return 0;
        }

        if (vm.count("version") > 0) {
            print_version_info();
            return 0;
        }

        if (vm.count("dump-config-template") > 0) {
            print_configuration_template();
            return 0;
        }

        if (vm.count("config-file") == 0) {
            fmt::print(stderr, "Error: Missing [CONFIG_FILE_PATH] parameter.");
            return 1;
        }

        const auto config = json::parse(std::ifstream{vm["config-file"].as<std::string>()});
        irods::http::globals::set_configuration(config);

        const auto& http_server_config = config.at("http_server");
        set_log_level(http_server_config);
        spdlog::set_pattern("[%Y-%m-%d %T.%e] [P:%P] [%^%l%$] [T:%t] %v");

        log::info("Initializing server.");

        // TODO For LONG running tasks, see the following:
        //
        //   - https://stackoverflow.com/questions/17648725/long-running-blocking-operations-in-boost-asio-handlers
        //   - https://www.open-std.org/JTC1/SC22/WG21/docs/papers/2012/n3388.pdf
        //

        log::trace("Loading API plugins.");
        load_client_api_plugins();

        const auto address = net::ip::make_address(http_server_config.at("host").get_ref<const std::string&>());
        const auto port = http_server_config.at("port").get<std::uint16_t>();
        const auto request_thread_count = std::max(http_server_config.at(json::json_pointer{"/requests/threads"}).get<int>(), 1);

        log::trace("Initializing TLS.");
        init_tls(config);

        // The io_context is required for all I/O.
        log::trace("Initializing HTTP components.");
        net::io_context ioc{request_thread_count};
        irods::http::globals::set_request_handler_io_context(ioc);

        // Create and launch a listening port.
        log::trace("Initializing listening socket (host=[{}], port=[{}]).", address.to_string(), port);
        std::make_shared<listener>(ioc, tcp::endpoint{address, port}, config)->run();

        // Capture SIGINT and SIGTERM to perform a clean shutdown.
        log::trace("Initializing signal handlers.");
        net::signal_set signals{ioc, SIGINT, SIGTERM};
        signals.async_wait([&ioc](const beast::error_code&, int _signal) {
            // Stop the io_context. This will cause run() to return immediately, eventually destroying the
            // io_context and all of the sockets in it.
            log::warn("Received signal [{}]. Shutting down.", _signal);
            ioc.stop();
        });

        // Launch the requested number of dedicated backgroup I/O threads.
        // These threads are used for long running tasks (e.g. reading/writing bytes, database, etc.)
        log::trace("Initializing thread pool for long running I/O tasks.");
        net::thread_pool io_threads(std::max(http_server_config.at(json::json_pointer{"/background_io/threads"}).get<int>(), 1));
        irods::http::globals::set_background_thread_pool(io_threads);

        // Run the I/O service on the requested number of threads.
        log::trace("Initializing thread pool for HTTP requests.");
        net::thread_pool request_handler_threads(request_thread_count);
        for (auto i = request_thread_count - 1; i > 0; --i) {
            net::post(request_handler_threads, [&ioc] { ioc.run(); });
        }

        // Launch eviction check for expired bearer tokens.
        const auto eviction_check_interval = http_server_config.at(json::json_pointer{"/authentication/eviction_check_interval_in_seconds"}).get<int>();
        bearer_token_eviction_manager eviction_mgr{ioc, std::chrono::seconds{eviction_check_interval}};

        log::info("Server is ready.");
        ioc.run();

        request_handler_threads.stop();
        io_threads.stop();

        log::trace("Waiting for HTTP requests thread pool to shut down.");
        request_handler_threads.join();

        log::trace("Waiting for I/O thread pool to shut down.");
        io_threads.join();

        log::info("Shutdown complete.");

        return 0;
    }
    catch (const irods::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.client_display_what());
        return 1;
    }
    catch (const std::exception& e) {
        fmt::print(stderr, "Error: {}\n", e.what());
        return 1;
    }
} // main
