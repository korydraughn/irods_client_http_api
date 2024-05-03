#ifndef IRODS_HTTP_API_CRLF_PARSER_HPP
#define IRODS_HTTP_API_CRLF_PARSER_HPP

#include "irods/private/http_api/common.hpp"

#include <optional>
#include <string_view>

namespace irods::http
{
	class crlf_parser
	{
	  public:
		explicit crlf_parser(const std::string_view _s)
			: data_{_s}
		{
		}

		auto next_crlf(std::int64_t _count = -1) -> bool
		{
			if (spos_ == 0 && epos_ == 0) {
				epos_ = data_.find("\r\n");
			}
			else if (epos_ != std::string_view::npos) {
				// TODO +2 can lead to wrapping if epos_ is unsigned.
				spos_ = std::clamp(epos_ + 2, epos_, std::string_view::npos); // Skip CRLF.

				if (_count > -1) {
					epos_ = spos_ + _count;
				}
				else {
					if (const auto pos = data_.find("\r\n", spos_); std::string_view::npos != pos) {
						epos_ = pos;
					}
					else {
						epos_ = data_.size();
						return false;
					}
				}
			}

			return std::string_view::npos != epos_;
		}

		auto next_crlf_start_boundary(const std::string_view _start_boundary) -> bool
		{
			// TODO +2 can lead to wrapping if epos_ is unsigned.
			spos_ = std::clamp(epos_ + 2, epos_, std::string_view::npos); // Skip CRLF.

			const auto bpos = data_.find(_start_boundary, spos_);

			// Boundary not found.
			if (bpos == std::string_view::npos) {
				epos_ = data_.size();
				return false;
			}

			// Assume the previous two bytes represent "\r\n".
			// Move the end position to the "\r\n".
			epos_ = bpos - 2;

			return true;
		}

		auto data() -> std::string_view
		{
			return data_.substr(spos_, epos_ - spos_);
		}

	  private:
		const std::string_view data_;
		std::string_view::size_type spos_ = 0;
		std::string_view::size_type epos_ = 0;
	}; // class crlf_parser

	auto get_multipart_form_data_boundary(const std::string_view _data) -> std::optional<std::string_view>;

	auto parse_multipart_form_data(const std::string_view _boundary, const std::string_view _data)
		-> query_arguments_type;
} // namespace irods::http

#endif // IRODS_HTTP_API_CRLF_PARSER_HPP
