#include "common.hpp"

#include "globals.hpp"
#include "log.hpp"
#include "version.hpp"

#include <irods/irods_exception.hpp>
#include <irods/process_stash.hpp>
#include <irods/rcMisc.h> // For addKeyVal().
#include <irods/rodsErrorTable.h>
#include <irods/rodsKeyWdDef.h> // For KW_CLOSE_OPEN_REPLICAS.

#include <boost/any.hpp>
#include <boost/algorithm/string.hpp>

#include <curl/curl.h>
#include <fmt/format.h>
#include <json.hpp>
#include <spdlog/spdlog.h>

namespace irods::http
{
    auto fail(response_type& _response, status_type _status, const std::string_view _error_msg) -> response_type
    {
        _response.result(_status);
        _response.set(field_type::server, version::server_name);
        _response.set(field_type::content_type, "application/json");
        _response.body() = _error_msg;
        _response.prepare_payload();
        return _response;
    } // fail

    auto fail(response_type& _response, status_type _status) -> response_type
    {
        return fail(_response, _status, "");
    } // fail

    auto fail(status_type _status, const std::string_view _error_msg) -> response_type
    {
        response_type r{_status, 11};
        return fail(r, _status, _error_msg);
    } // fail

    auto fail(status_type _status) -> response_type
    {
        response_type r{_status, 11};
        return fail(r, _status, "");
    } // fail

    auto decode(const std::string_view _v) -> std::string
    {
        std::string result;
        int decoded_length = -1;

        if (auto* decoded = curl_easy_unescape(nullptr, _v.data(), static_cast<int>(_v.size()), &decoded_length); decoded) {
            std::unique_ptr<char, void(*)(void*)> s{decoded, curl_free};
            result.assign(decoded, decoded_length);
        }
        else {
            result.assign(_v);
        }

        return result;
    } // decode

    // TODO Create a better name.
    auto to_argument_list(const std::string_view _urlencoded_string) -> std::unordered_map<std::string, std::string>
    {
        if (_urlencoded_string.empty()) {
            return {};
        }

        std::unordered_map<std::string, std::string> kvps;

        std::vector<std::string> tokens;
        boost::split(tokens, _urlencoded_string, boost::is_any_of("&"));

        std::vector<std::string> kvp;

        for (auto&& t : tokens) {
            boost::split(kvp, t, boost::is_any_of("="));

            if (kvp.size() == 2) {
                auto value = decode(kvp[1]);
                boost::replace_all(value, "+", " ");
                kvps.insert_or_assign(std::move(kvp[0]), value);
            }
            else if (kvp.size() == 1) {
                kvps.insert_or_assign(std::move(kvp[0]), "");
            }

            kvp.clear();
        }

        return kvps;
    } // to_argument_list

    auto get_url_path(const std::string& _url) -> std::optional<std::string>
    {
        namespace log = irods::http::log;

        std::unique_ptr<CURLU, void(*)(CURLU*)> curl{curl_url(), curl_url_cleanup};

        if (!curl) {
            log::error("{}: Could not initialize libcurl.", __func__);
            return std::nullopt;
        }

        if (const auto ec = curl_url_set(curl.get(), CURLUPART_URL, _url.c_str(), 0); ec) {
            log::error("{}: curl_url_set error: {}", __func__, ec);
            return std::nullopt;
        }

        using curl_string = std::unique_ptr<char, void(*)(void*)>;

        // Extract the path.
        // This is what we use to route requests to the various endpoints.
        char* path{};
        const auto ec = curl_url_get(curl.get(), CURLUPART_PATH, &path, 0);

        if (ec == 0) {
            curl_string cpath{path, curl_free};
            return path;
        }

        log::error("{}: curl_url_get(CURLUPART_PATH) error: {}", __func__, ec);
        return std::nullopt;
    } // get_url_path

    auto parse_url(const std::string& _url) -> url
    {
        namespace log = irods::http::log;

        std::unique_ptr<CURLU, void(*)(CURLU*)> curl{curl_url(), curl_url_cleanup};

        if (!curl) {
            log::error("{}: Could not initialize CURLU handle.", __func__);
            THROW(SYS_LIBRARY_ERROR, "curl_url error.");
        }

        // Include a bogus prefix. We only care about the path and query parts of the URL.
        if (const auto ec = curl_url_set(curl.get(), CURLUPART_URL, _url.c_str(), 0); ec) {
            log::error("{}: curl_url_set error: {}", __func__, ec);
            THROW(SYS_LIBRARY_ERROR, "curl_url_set(CURLUPART_URL) error.");
        }

        url url;

        using curl_string = std::unique_ptr<char, void(*)(void*)>;

        // Extract the path.
        // This is what we use to route requests to the various endpoints.
        char* path{};
        if (const auto ec = curl_url_get(curl.get(), CURLUPART_PATH, &path, 0); ec == 0) {
            curl_string cpath{path, curl_free};
            if (path) {
                url.path = path;
            }
        }
        else {
            log::error("{}: curl_url_get(CURLUPART_PATH) error: {}", __func__, ec);
            THROW(SYS_LIBRARY_ERROR, "curl_url_get(CURLUPART_PATH) error.");
        }

        // Extract the query.
        // ChatGPT states that the values in the key value pairs must escape embedded equal signs.
        // This allows the HTTP server to parse the query string correctly. Therefore, we don't have
        // to protect against that case. The client must send the correct URL escaped input.
        char* query{};
        if (const auto ec = curl_url_get(curl.get(), CURLUPART_QUERY, &query, 0); ec == 0) {
            curl_string cs{query, curl_free};
            if (query) {
                url.query = to_argument_list(query);
            }
        }
        else {
            log::error("{}: curl_url_get(CURLUPART_QUERY) error: {}", __func__, ec);
            THROW(SYS_LIBRARY_ERROR, "curl_url_get(CURLUPART_QUERY) error.");
        }

        return url;
    } // parse_url

    auto parse_url(const request_type& _req) -> url
    {
        return parse_url(fmt::format("http://ignored{}", _req.target()));
    } // parse_url

    auto resolve_client_identity(const request_type& _req) -> client_identity_resolution_result
    {
        namespace log = irods::http::log;

        //
        // Extract the Bearer token from the Authorization header.
        //

        const auto& hdrs = _req.base();
        const auto iter = hdrs.find("Authorization");
        if (iter == std::end(hdrs)) {
            log::error("{}: Missing [Authorization] header.", __func__);
            return {.response = fail(status_type::bad_request)};
        }

        log::debug("{}: Authorization value: [{}]", __func__, iter->value());

        auto pos = iter->value().find("Bearer ");
        if (std::string_view::npos == pos) {
            log::debug("{}: Malformed authorization header.", __func__);
            return {.response = fail(status_type::bad_request)};
        }

        std::string bearer_token{iter->value().substr(pos + 7)};
        boost::trim(bearer_token);
        log::debug("{}: Bearer token: [{}]", __func__, bearer_token);

        // Verify the bearer token is known to the server. If not, return an error.
        const auto* object = irods::process_stash::find(bearer_token);
        if (!object) {
            log::error("{}: Could not find bearer token matching [{}].", __func__, bearer_token);
            return {.response = fail(status_type::unauthorized)};
        }

        const auto* p = boost::any_cast<authenticated_client_info>(&*object);
        if (std::chrono::steady_clock::now() >= p->expires_at) {
            log::error("{}: Session for bearer token [{}] has expired.", __func__, bearer_token);
            return {.response = fail(status_type::unauthorized)};
        }

        log::trace("{}: Client is authenticated.", __func__);
        return {.client_info = p};
    } // resolve_client_identity
} // namespace irods::http

namespace irods
{
    auto to_permission_string(const irods::experimental::filesystem::perms _p) -> const char*
    {
        using irods::experimental::filesystem::perms;

        switch (_p) {
            case perms::null:            return "null";
            case perms::read_metadata:   return "read_metadata";
            case perms::read_object:     return "read_object";
            case perms::read:            return "read_object";
            case perms::create_metadata: return "create_metadata";
            case perms::modify_metadata: return "modify_metadata";
            case perms::delete_metadata: return "delete_metadata";
            case perms::create_object:   return "create_object";
            case perms::modify_object:   return "modify_object";
            case perms::write:           return "modify_object";
            case perms::delete_object:   return "delete_object";
            case perms::own:             return "own";
            default:                     return "?"; // TODO std::unreachable() or __builtin_unreachable()
        }
    } // to_permission_string

    auto to_permission_enum(const std::string_view _s) -> std::optional<irods::experimental::filesystem::perms>
    {
        using irods::experimental::filesystem::perms;

        // clang-format off
        if (_s == "null")            { return perms::null; }
        if (_s == "read_metadata")   { return perms::read_metadata; }
        if (_s == "read_object")     { return perms::read_object; }
        if (_s == "red")             { return perms::read; }
        if (_s == "create_metadata") { return perms::create_metadata; }
        if (_s == "modify_metadata") { return perms::modify_metadata; }
        if (_s == "delete_metadata") { return perms::delete_metadata; }
        if (_s == "create_object")   { return perms::create_object; }
        if (_s == "modify_object")   { return perms::modify_object; }
        if (_s == "write")           { return perms::write; }
        if (_s == "delete_object")   { return perms::delete_object; }
        if (_s == "own")             { return perms::own; }
        // clang-format on

        return std::nullopt;
    } // to_permission_enum

    auto to_object_type_string(const irods::experimental::filesystem::object_type _t) -> const char*
    {
        using irods::experimental::filesystem::object_type;

        switch (_t) {
            case object_type::collection:         return "collection";
            case object_type::data_object:        return "data_object";
            case object_type::none:               return "none";
            case object_type::not_found:          return "not_found";
            case object_type::special_collection: return "special_collection";
            case object_type::unknown:            return "unknown";
            default:                              return "?";
        }
    } // to_object_type_string

    auto to_object_type_enum(const std::string_view _s) -> std::optional<irods::experimental::filesystem::object_type>
    {
        using irods::experimental::filesystem::object_type;

        // clang-format off
        if (_s == "collection")         { return object_type::collection; }
        if (_s == "data_object")        { return object_type::data_object; }
        if (_s == "none")               { return object_type::none; }
        if (_s == "not_found")          { return object_type::not_found; }
        if (_s == "special_collection") { return object_type::special_collection; }
        if (_s == "unknown")            { return object_type::unknown; }
        // clang-format on

        return std::nullopt;
    } // to_object_type_enum

    auto get_connection(const std::string& _username) -> irods::experimental::client_connection
    {
        namespace log = irods::http::log;
        using json_ptr = nlohmann::json::json_pointer;

        static const auto& host = irods::http::globals::configuration().at(json_ptr{"/irods_client/host"}).get_ref<const std::string&>();
        static const auto& port = irods::http::globals::configuration().at(json_ptr{"/irods_client/port"}).get<int>();
        static const auto& zone = irods::http::globals::configuration().at(json_ptr{"/irods_client/zone"}).get_ref<const std::string&>();
        static const auto& rodsadmin_username = irods::http::globals::configuration().at(json_ptr{"/irods_client/rodsadmin/username"}).get_ref<const std::string&>();
        static const auto& rodsadmin_password = irods::http::globals::configuration().at(json_ptr{"/irods_client/rodsadmin/password"}).get_ref<const std::string&>();

        log::trace("{}: Connecting to server as [{}].", __func__, _username);

        irods::experimental::client_connection conn{
            irods::experimental::defer_authentication, host, port, {rodsadmin_username, zone}, {_username, zone}};

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        if (const auto ec = clientLoginWithPassword(static_cast<RcComm*>(conn), const_cast<char*>(rodsadmin_password.c_str())); ec < 0) {
            auto msg = fmt::format("{}: Authentication error for user [{}]. Error code [{}].", __func__, _username, ec);
            log::error(msg);
            THROW(ec, std::move(msg));
        }

        log::trace("{}: Successfully to server as [{}].", __func__, _username);

        return conn;
    } // get_connection

    auto fail(boost::beast::error_code ec, char const* what) -> void
    {
        irods::http::log::error("{}: {}: {}", __func__, what, ec.message());
    } // fail
} // namespace irods
