#include "handlers.hpp"

#include "common.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "session.hpp"
#include "shared_api_operations.hpp"
#include "version.hpp"

#include <irods/irods_exception.hpp>
#include <irods/query_builder.hpp>
#include <irods/resource_administration.hpp>
#include <irods/rodsErrorTable.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// clang-format off
namespace beast = boost::beast;     // from <boost/beast.hpp>
namespace http  = beast::http;      // from <boost/beast/http.hpp>
namespace net   = boost::asio;      // from <boost/asio.hpp>

namespace adm = irods::experimental::administration;
namespace log = irods::http::log;

using json = nlohmann::json;
// clang-format on

#define IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(name) \
  auto name(                                              \
	  irods::http::session_pointer_type _sess_ptr,        \
	  irods::http::request_type& _req,                    \
	  irods::http::query_arguments_type& _args)           \
	  ->void

namespace
{
	using handler_type =
		void (*)(irods::http::session_pointer_type, irods::http::request_type&, irods::http::query_arguments_type&);

	//
	// Handler function prototypes
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_child);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_child);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_rebalance);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat);
	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata);

	//
	// Operation to Handler mappings
	//

	// clang-format off
	const std::unordered_map<std::string, handler_type> handlers_for_get{
		{"stat", op_stat}
	};

	const std::unordered_map<std::string, handler_type> handlers_for_post{
		{"create", op_create},
		{"remove", op_remove},
		{"modify", op_modify},
		{"add_child", op_add_child},
		{"remove_child", op_remove_child},
		{"rebalance", op_rebalance},
		{"modify_metadata", op_modify_metadata}
	};
	// clang-format on
} // anonymous namespace

namespace irods::http::handler
{
	// NOLINTNEXTLINE(performance-unnecessary-value-param)
	IRODS_HTTP_API_ENDPOINT_ENTRY_FUNCTION_SIGNATURE(resources)
	{
		if (_req.method() == verb_type::get) {
			auto url = irods::http::parse_url(_req);

			const auto op_iter = url.query.find("op");
			if (op_iter == std::end(url.query)) {
				log::error("{}: Missing [op] parameter.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::bad_request));
			}

			if (const auto iter = handlers_for_get.find(op_iter->second); iter != std::end(handlers_for_get)) {
				return (iter->second)(_sess_ptr, _req, url.query);
			}

			return _sess_ptr->send(fail(status_type::bad_request));
		}

		if (_req.method() == verb_type::post) {
			auto args = irods::http::to_argument_list(_req.body());

			const auto op_iter = args.find("op");
			if (op_iter == std::end(args)) {
				log::error("{}: Missing [op] parameter.", __func__);
				return _sess_ptr->send(irods::http::fail(status_type::bad_request));
			}

			if (const auto iter = handlers_for_post.find(op_iter->second); iter != std::end(handlers_for_post)) {
				return (iter->second)(_sess_ptr, _req, args);
			}

			return _sess_ptr->send(fail(status_type::bad_request));
		}

		log::error("{}: Incorrect HTTP method.", __func__);
		return _sess_ptr->send(irods::http::fail(status_type::method_not_allowed));
	} // resources
} // namespace irods::http::handler

namespace
{
	//
	// Operation handler implementations
	//

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_create)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				log::info("{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						log::error("{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					const auto type_iter = _args.find("type");
					if (type_iter == std::end(_args)) {
						log::error("{}: Missing [type] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					adm::resource_registration_info resc_info;
					resc_info.resource_name = name_iter->second;
					resc_info.resource_type = type_iter->second;

					const auto host_iter = _args.find("host");
					if (host_iter != std::end(_args)) {
						resc_info.host_name = host_iter->second;
					}

					const auto vault_path_iter = _args.find("vault-path");
					if (vault_path_iter != std::end(_args)) {
						resc_info.vault_path = vault_path_iter->second;
					}

					const auto ctx_iter = _args.find("context");
					if (ctx_iter != std::end(_args)) {
						resc_info.context_string = ctx_iter->second;
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::add_resource(conn, resc_info);

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					res.result(http::status::bad_request);
					res.body() =
						json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
							.dump();
				}
				catch (const std::exception& e) {
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_create

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				log::info("{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						log::error("{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::remove_resource(conn, name_iter->second);

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					res.result(http::status::bad_request);
					res.body() =
						json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
							.dump();
				}
				catch (const std::exception& e) {
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_remove

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify)
	{
#if 0
        auto result = irods::http::resolve_client_identity(_req);
        if (result.response) {
            return _sess_ptr->send(std::move(*result.response));
        }

        const auto client_info = result.client_info;

        irods::http::globals::background_task([fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
            log::info("{}: client_info.username = [{}]", fn, client_info.username);

            http::response<http::string_body> res{http::status::ok, _req.version()};
            res.set(http::field::server, irods::http::version::server_name);
            res.set(http::field::content_type, "application/json");
            res.keep_alive(_req.keep_alive());

            try {
            }
            catch (const irods::exception& e) {
                res.result(http::status::bad_request);
                res.body() = json{
                    {"irods_response", {
                        {"status_code", e.code()},
                        {"status_message", e.client_display_what()}
                    }}
                }.dump();
            }
            catch (const std::exception& e) {
                res.result(http::status::internal_server_error);
            }

            res.prepare_payload();

            return _sess_ptr->send(std::move(res));
        });
#else
		(void) _req;
		(void) _args;
		log::error("{}: Operation not implemented.", __func__);
		return _sess_ptr->send(irods::http::fail(http::status::not_implemented));
#endif
	} // op_modify

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_add_child)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				log::info("{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto parent_name_iter = _args.find("parent-name");
					if (parent_name_iter == std::end(_args)) {
						log::error("{}: Missing [parent-name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					const auto child_name_iter = _args.find("child-name");
					if (child_name_iter == std::end(_args)) {
						log::error("{}: Missing [child-name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);

					const auto ctx_iter = _args.find("context");
					if (ctx_iter != std::end(_args)) {
						adm::client::add_child_resource(
							conn, parent_name_iter->second, child_name_iter->second, ctx_iter->second);
					}
					else {
						adm::client::add_child_resource(conn, parent_name_iter->second, child_name_iter->second);
					}

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					res.result(http::status::bad_request);
					res.body() =
						json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
							.dump();
				}
				catch (const std::exception& e) {
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_add_child

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_remove_child)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				log::info("{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto parent_name_iter = _args.find("parent-name");
					if (parent_name_iter == std::end(_args)) {
						log::error("{}: Missing [parent-name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					const auto child_name_iter = _args.find("child-name");
					if (child_name_iter == std::end(_args)) {
						log::error("{}: Missing [child-name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::remove_child_resource(conn, parent_name_iter->second, child_name_iter->second);

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					res.result(http::status::bad_request);
					res.body() =
						json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
							.dump();
				}
				catch (const std::exception& e) {
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_remove_child

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_rebalance)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task(
			[fn = __func__, client_info, _sess_ptr, _req = std::move(_req), _args = std::move(_args)] {
				log::info("{}: client_info.username = [{}]", fn, client_info.username);

				http::response<http::string_body> res{http::status::ok, _req.version()};
				res.set(http::field::server, irods::http::version::server_name);
				res.set(http::field::content_type, "application/json");
				res.keep_alive(_req.keep_alive());

				try {
					const auto name_iter = _args.find("name");
					if (name_iter == std::end(_args)) {
						log::error("{}: Missing [name] parameter.", fn);
						return _sess_ptr->send(irods::http::fail(http::status::bad_request));
					}

					auto conn = irods::get_connection(client_info.username);
					adm::client::rebalance_resource(conn, name_iter->second);

					res.body() = json{
						{"irods_response",
				         {
							 {"status_code", 0},
						 }}}.dump();
				}
				catch (const irods::exception& e) {
					res.result(http::status::bad_request);
					res.body() =
						json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
							.dump();
				}
				catch (const std::exception& e) {
					res.result(http::status::internal_server_error);
				}

				res.prepare_payload();

				return _sess_ptr->send(std::move(res));
			});
	} // op_rebalance

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_stat)
	{
		auto result = irods::http::resolve_client_identity(_req);
		if (result.response) {
			return _sess_ptr->send(std::move(*result.response));
		}

		const auto client_info = result.client_info;

		irods::http::globals::background_task([fn = __func__,
		                                       client_info,
		                                       _sess_ptr,
		                                       _req = std::move(_req),
		                                       _args = std::move(_args)] {
			log::info("{}: client_info.username = [{}]", fn, client_info.username);

			http::response<http::string_body> res{http::status::ok, _req.version()};
			res.set(http::field::server, irods::http::version::server_name);
			res.set(http::field::content_type, "application/json");
			res.keep_alive(_req.keep_alive());

			try {
				const auto name_iter = _args.find("name");
				if (name_iter == std::end(_args)) {
					log::error("{}: Missing [name] parameter.", fn);
					return _sess_ptr->send(irods::http::fail(http::status::bad_request));
				}

				auto conn = irods::get_connection(client_info.username);

				json::object_t info;
				bool exists = false;

				const auto& config = irods::http::globals::configuration();

				if (config.at(json::json_pointer{"/irods_client/enable_4_2_compatibility"}).get<bool>()) {
					const auto gql = fmt::format(
						"select RESC_ID, RESC_TYPE_NAME, RESC_ZONE_NAME, "
						"RESC_LOC, RESC_VAULT_PATH, RESC_STATUS, "
						"RESC_CONTEXT, RESC_COMMENT, RESC_INFO, "
						"RESC_FREE_SPACE, RESC_FREE_SPACE_TIME, "
						"RESC_PARENT, RESC_CREATE_TIME, RESC_MODIFY_TIME "
						"where RESC_NAME = '{}'",
						name_iter->second);

					for (auto&& row : irods::experimental::query_builder{}.build<RcComm>(conn, gql)) {
						exists = true;

						// clang-format off
						info = {
							{"id", row[0]},
							{"name", name_iter->second},
							{"type", row[1]},
							{"zone", row[2]},
							{"host", row[3]},
							{"vault_path", row[4]},
							{"status", adm::to_resource_status(row[5])},
							{"context", row[6]},
							{"comments", row[7]},
							{"information", row[8]},
							{"free_space", row[9]},
							{"free_space_last_modified", 0},
							{"parent_id", row[11]},
							{"created", std::stoull(row[12])},
							{"last_modified", std::stoull(row[13])},
							{"last_modified_millis", 0}
						};
						// clang-format on

						if (!row[10].empty()) {
							info["free_space_last_modified"] = std::stoull(row[10]);
						}
					}
				}
				else if (const auto resc = adm::client::resource_info(conn, name_iter->second); resc) {
					exists = true;

					// clang-format off
					info = {
						{"id", resc->id()},
						{"name", resc->name()},
						{"type", resc->type()},
						{"zone", resc->zone_name()},
						{"host", resc->host_name()},
						{"vault_path", resc->vault_path()},
						{"status", resc->status()},
						{"context", resc->context_string()},
						{"comments", resc->comments()},
						{"information", resc->information()},
						{"free_space", resc->free_space()},
						{"free_space_last_modified", resc->free_space_last_modified().time_since_epoch().count()},
						{"parent_id", resc->parent_id()},
						{"created", resc->created().time_since_epoch().count()},
						{"last_modified", resc->last_modified().time_since_epoch().count()},
						{"last_modified_millis", resc->last_modified_millis().count()}
					};
					// clang-format on
				}

				res.body() = json{{"irods_response", {{"status_code", 0}}}, {"exists", exists}, {"info", info}}.dump();
			}
			catch (const irods::exception& e) {
				res.result(http::status::bad_request);
				res.body() =
					json{{"irods_response", {{"status_code", e.code()}, {"status_message", e.client_display_what()}}}}
						.dump();
			}
			catch (const std::exception& e) {
				res.result(http::status::internal_server_error);
			}

			res.prepare_payload();

			return _sess_ptr->send(std::move(res));
		});
	} // op_stat

	IRODS_HTTP_API_ENDPOINT_OPERATION_SIGNATURE(op_modify_metadata)
	{
		using namespace irods::http::shared_api_operations;
		return op_atomic_apply_metadata_operations(_sess_ptr, _req, _args, entity_type::resource);
	} // op_modify_metadata
} // anonymous namespace
