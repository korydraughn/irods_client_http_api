#ifndef IRODS_HTTP_API_LOG_HPP
#define IRODS_HTTP_API_LOG_HPP

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string_view>
#include <utility>

namespace irods::http::log
{
	inline auto trace(const std::string_view _msg) -> void
	{
		spdlog::trace(_msg);
	} // trace

	inline auto info(const std::string_view _msg) -> void
	{
		spdlog::info(_msg);
	} // trace

	inline auto debug(const std::string_view _msg) -> void
	{
		spdlog::debug(_msg);
	} // trace

	inline auto warn(const std::string_view _msg) -> void
	{
		spdlog::warn(_msg);
	} // trace

	inline auto error(const std::string_view _msg) -> void
	{
		spdlog::error(_msg);
	} // trace

	inline auto critical(const std::string_view _msg) -> void
	{
		spdlog::critical(_msg);
	} // trace

	template <typename... Args>
	constexpr auto trace(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::trace(_format, std::forward<Args>(_args)...);
	} // trace

	template <typename... Args>
	constexpr auto info(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::info(_format, std::forward<Args>(_args)...);
	} // info

	template <typename... Args>
	constexpr auto debug(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::debug(_format, std::forward<Args>(_args)...);
	} // debug

	template <typename... Args>
	constexpr auto warn(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::warn(_format, std::forward<Args>(_args)...);
	} // warn

	template <typename... Args>
	constexpr auto error(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::error(_format, std::forward<Args>(_args)...);
	} // error

	template <typename... Args>
	constexpr auto critical(fmt::format_string<Args...> _format, Args&&... _args) -> void
	{
		spdlog::critical(_format, std::forward<Args>(_args)...);
	} // critical

	class operation_logger
	{
	public:
		explicit constexpr operation_logger(std::string_view _ip, std::string_view _op)
			: prefix_{fmt::format("[{}] [{}]", _ip, _op)}
		{
		}

		// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define IRODS_HTTP_LOG_ARGS "{} {}", prefix_, fmt::format(fmt::runtime(_format), std::forward<Args>(_args)...)

		template <typename... Args>
		constexpr auto trace(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::trace(IRODS_HTTP_LOG_ARGS);
		} // trace

		template <typename... Args>
		constexpr auto info(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::info(IRODS_HTTP_LOG_ARGS);
		} // info

		template <typename... Args>
		constexpr auto debug(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::debug(IRODS_HTTP_LOG_ARGS);
		} // debug

		template <typename... Args>
		constexpr auto warn(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::warn(IRODS_HTTP_LOG_ARGS);
		} // warn

		template <typename... Args>
		constexpr auto error(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::error(IRODS_HTTP_LOG_ARGS);
		} // error

		template <typename... Args>
		constexpr auto critical(const std::string_view _format, Args&&... _args) -> void
		{
			spdlog::critical(IRODS_HTTP_LOG_ARGS);
		} // critical

#undef IRODS_HTTP_LOG_ARGS

	private:
		std::string prefix_;
	}; // class operation_logger
} //namespace irods::http::log

#endif // IRODS_HTTP_API_LOG_HPP
