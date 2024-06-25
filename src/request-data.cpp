#include <inja/inja.hpp>

#include "request-data.h"
#include "plugin-support.h"
#include "parsers/parsers.h"
#include "string-util.h"

#include <cstddef>
#include <string>
#include <fstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <locale>

#ifdef _WIN32
#include <windows.h>
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <util/base.h>
#include <obs-module.h>

#include "obs-source-util.h"

#define URL_SOURCE_AGG_BUFFER_MAX_SIZE 1024

static const std::string USER_AGENT = std::string(PLUGIN_NAME) + "/" + std::string(PLUGIN_VERSION);

std::size_t writeFunctionStdString(void *ptr, std::size_t size, size_t nmemb, std::string *data)
{
	data->append(static_cast<char *>(ptr), size * nmemb);
	return size * nmemb;
}

std::size_t writeFunctionUint8Vector(void *ptr, std::size_t size, size_t nmemb,
				     std::vector<uint8_t> *data)
{
	data->insert(data->end(), static_cast<uint8_t *>(ptr),
		     static_cast<uint8_t *>(ptr) + size * nmemb);
	return size * nmemb;
}

bool hasOnlyValidURLCharacters(const std::string &url)
{
	// This pattern allows typical URL characters including percent encoding
	const std::regex pattern(R"(^[A-Za-z0-9\-._~:/?#\[\]@!$&'()*+,;=%]+$)");
	return std::regex_match(url, pattern);
}

size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	std::map<std::string, std::string> *headers =
		static_cast<std::map<std::string, std::string> *>(userdata);
	size_t real_size = size * nitems;
	std::string header_line(buffer, real_size);

	// Find the colon in the line
	auto pos = header_line.find(':');
	if (pos != std::string::npos) {
		std::string key = header_line.substr(0, pos);
		std::string value = header_line.substr(pos + 1);

		// Remove potential carriage return
		if (!value.empty() && value.back() == '\r') {
			value.pop_back();
		}

		// Convert key to lowercase
		std::transform(key.begin(), key.end(), key.begin(),
			       [](unsigned char c) { return std::tolower(c); });

		// Store the header
		(*headers)[key] = value;
	}

	return real_size;
}

void handle_nonempty_text(input_data &input, request_data_handler_response &response,
			  nlohmann::json &json, const char *text)
{
	if (input.no_same) {
		if (input.last_obs_text_source_value == text) {
			// Return an error response
			response.error_message =
				"OBS text source value is the same as last time, skipping was requested";
			response.status_code = URL_SOURCE_REQUEST_BENIGN_ERROR_CODE;
			return;
		}
		input.last_obs_text_source_value = text;
	}
	if (input.aggregate) {
		// aggregate to target is requested and the text is not empty

		// if the buffer ends with a punctuation mark, remove it
		if (input.aggregate_to_empty_buffer.size() > 0) {
			char lastChar = input.aggregate_to_empty_buffer.back();
			if (lastChar == '.' || lastChar == ',' || lastChar == '!' ||
			    lastChar == '?') {
				input.aggregate_to_empty_buffer.pop_back();
				// insert a space where the punctuation mark was
				input.aggregate_to_empty_buffer += " ";
			}
		}

		// trim the text and add it to the aggregate buffer
		std::string textStr = text;
		input.aggregate_to_empty_buffer += trim(textStr);
		// if the buffer is larger than the limit, remove the first part
		if (input.aggregate_to_empty_buffer.size() > URL_SOURCE_AGG_BUFFER_MAX_SIZE) {
			input.aggregate_to_empty_buffer.erase(
				0, input.aggregate_to_empty_buffer.size() -
					   URL_SOURCE_AGG_BUFFER_MAX_SIZE);
		}

		if (input.agg_method != URL_SOURCE_AGG_TARGET_EMPTY) {
			// this is a non-empty aggregate *timed* target
			if (input.agg_buffer_begin_ts == 0) {
				// this is the first time we aggregate, set the timer
				input.agg_buffer_begin_ts = get_time_ns();
			}
			// check if the agg timer has expired
			const uint64_t timer_interval_ns =
				url_source_agg_target_to_nanoseconds(input.agg_method);
			if ((get_time_ns() - input.agg_buffer_begin_ts) >= timer_interval_ns) {
				// aggregate timer has expired, use the aggregate buffer
				obs_log(LOG_INFO,
					"Aggregate timer expired, using aggregate buffer (len %d)",
					input.aggregate_to_empty_buffer.size());
				json["input"] = input.aggregate_to_empty_buffer;
				input.aggregate_to_empty_buffer = "";
				input.agg_buffer_begin_ts = get_time_ns();
			} else {
				// aggregate timer has not expired, return an error response
				response.error_message =
					"Aggregate timer has not expired, skipping";
				response.status_code = URL_SOURCE_REQUEST_BENIGN_ERROR_CODE;
			}
		} else {
			response.error_message = "Aggregate to empty is requested, skipping";
			response.status_code = URL_SOURCE_REQUEST_BENIGN_ERROR_CODE;
		}
	}
}

void handle_empty_text(input_data &input, request_data_handler_response &response,
		       nlohmann::json &json)
{
	if (input.agg_method == URL_SOURCE_AGG_TARGET_EMPTY &&
	    !input.aggregate_to_empty_buffer.empty()) {
		// empty input found, use the aggregate buffer if it's not empty
		obs_log(LOG_INFO, "OBS text source is empty, using aggregate buffer (len %d)",
			input.aggregate_to_empty_buffer.size());
		json["input"] = input.aggregate_to_empty_buffer;
		input.aggregate_to_empty_buffer = "";
		input.agg_buffer_begin_ts = get_time_ns();
	} else if (input.no_empty) {
		// Return an error response
		response.error_message = "OBS text source is empty, skipping was requested";
		response.status_code = URL_SOURCE_REQUEST_BENIGN_ERROR_CODE;
	}
}

void put_inputs_on_json(url_source_request_data *request_data, CURL *curl,
			request_data_handler_response &response, nlohmann::json &json)
{
	for (size_t i = 0; i < request_data->inputs.size(); i++) {
		// Add the input to the json object
		input_data &input = request_data->inputs[i];

		if (input.source == "") {
			// no dynamic data source is set
			continue;
		}

		// remove the prefix from the source name
		std::string source_name = get_source_name_without_prefix(input.source);
		// Check if the source is a text source
		if (is_obs_source_text(source_name)) {
			obs_source_t *source = obs_get_source_by_name(source_name.c_str());
			std::string textStr;
			if (source) {
				// Get text from OBS text source
				obs_data_t *sourceSettings = obs_source_get_settings(source);
				const char *text = obs_data_get_string(sourceSettings, "text");
				obs_data_release(sourceSettings);
				obs_source_release(source);
				if (text != NULL) {
					textStr = text;
				}
			}

			if (textStr.empty()) {
				handle_empty_text(input, response, json);
			} else {
				// trim the text for whistespace
				textStr = trim(textStr);

				handle_nonempty_text(input, response, json, textStr.c_str());

				// if one of the headers is Content-Type application/json, make sure the text is JSONified
				std::regex header_regex("content-type",
							std::regex_constants::icase);
				std::regex header_value_regex("application/json",
							      std::regex_constants::icase);
				for (auto header : request_data->headers) {
					// check if the header is Content-Type case insensitive using regex
					if (std::regex_search(header.first, header_regex) &&
					    std::regex_search(header.second, header_value_regex)) {
						nlohmann::json tmp = textStr;
						textStr = tmp.dump();
						// remove '"' from the beginning and end of the string
						textStr = textStr.substr(1, textStr.size() - 2);
						break;
					}
				}

				json["input" + std::to_string(i)] = textStr;
				if (i == 0) {
					// also add the 0'th input to the "input" key
					json["input"] = textStr;
				}
			}
			if (response.status_code == URL_SOURCE_REQUEST_BENIGN_ERROR_CODE) {
				curl_easy_cleanup(curl);
				return;
			}
		} else {
			// this is not a text source.
			// we should grab an image of its output, encode it to base64 and use it as input
			obs_source_t *source = obs_get_source_by_name(source_name.c_str());
			if (source == NULL) {
				obs_log(LOG_INFO, "Failed to get source by name");
				// Return an error response
				response.error_message = "Failed to get source by name";
				response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
				curl_easy_cleanup(curl);
				return;
			}

			// render the source to an image using get_rgba_from_source_render
			source_render_data tf;
			init_source_render_data(&tf);
			// get the scale factor from the request_data->obs_input_source_resize_option
			float scale = 1.0;
			if (input.resize_method != "100%") {
				// parse the scale from the string
				std::string scaleStr = input.resize_method;
				scaleStr.erase(std::remove(scaleStr.begin(), scaleStr.end(), '%'),
					       scaleStr.end());
				scale = (float)(std::stof(scaleStr) / 100.0f);
			}

			uint32_t width, height;
			std::vector<uint8_t> rgba =
				get_rgba_from_source_render(source, &tf, width, height, scale);
			if (rgba.empty()) {
				obs_log(LOG_INFO, "Failed to get RGBA from source render");
				// Return an error response
				response.error_message = "Failed to get RGBA from source render";
				response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
				curl_easy_cleanup(curl);
				return;
			}
			destroy_source_render_data(&tf);

			// encode the image to base64
			std::string base64 = convert_rgba_buffer_to_png_base64(rgba, width, height);

			// set the input to the base64 encoded image
			json["imageb64"] = base64;
		} // end of non-text source
	}
}

struct request_data_handler_response request_data_handler(url_source_request_data *request_data)
{
	struct request_data_handler_response response;

	request_data->sequence_number++;

	if (request_data->url_or_file == "file") {
		// This is a file request
		// Read the file
		std::ifstream file(request_data->url);
		if (!file.is_open()) {
			obs_log(LOG_INFO, "Failed to open file");
			// Return an error response
			response.error_message = "Failed to open file";
			response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
			return response;
		}
		std::string responseBody((std::istreambuf_iterator<char>(file)),
					 std::istreambuf_iterator<char>());
		file.close();

		response.body = responseBody;
		response.status_code = URL_SOURCE_REQUEST_SUCCESS;
	} else {
		// This is a URL request
		// Check if the URL is empty
		if (request_data->url == "") {
			obs_log(LOG_INFO, "URL is empty");
			// Return an error response
			response.error_message = "URL is empty";
			response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
			return response;
		}

		// Build the request with libcurl
		CURL *curl = curl_easy_init();
		if (!curl) {
			obs_log(LOG_INFO, "Failed to initialize curl");
			// Return an error response
			response.error_message = "Failed to initialize curl";
			response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
			return response;
		}
		curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT.c_str());
		if (request_data->fail_on_http_error) {
			curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
		}

		std::string responseBody;
		std::vector<uint8_t> responseBodyUint8;

		// if the request is for textual data write to string
		if (request_data->output_type == "JSON" ||
		    request_data->output_type == "XML (XPath)" ||
		    request_data->output_type == "XML (XQuery)" ||
		    request_data->output_type == "HTML" || request_data->output_type == "Text") {
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunctionStdString);
		} else {
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBodyUint8);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunctionUint8Vector);
		}

		if (request_data->headers.size() > 0) {
			// Add request headers
			struct curl_slist *headers = NULL;
			for (auto header : request_data->headers) {
				std::string header_string = header.first + ": " + header.second;
				headers = curl_slist_append(headers, header_string.c_str());
			}
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		}

		nlohmann::json json; // json object or variables for inja

		// Put the request inputs on the json object
		put_inputs_on_json(request_data, curl, response, json);

		if (response.status_code != URL_SOURCE_REQUEST_SUCCESS) {
			curl_easy_cleanup(curl);
			return response;
		}

		// Replace placeholders in the URL and body with the input values
		inja::Environment env;
		// Add an inja callback for time formatting
		env.add_callback("strftime", 2, [](inja::Arguments &args) {
			std::string format = args.at(0)->get<std::string>();
			std::time_t t = std::time(nullptr);
			std::tm *tm = std::localtime(&t);
			if (args.at(1)->get<bool>()) {
				// if the second argument is true, use UTC time
				tm = std::gmtime(&t);
			}
			char buffer[256];
			std::strftime(buffer, sizeof(buffer), format.c_str(), tm);
			return std::string(buffer);
		});
		// add a callback for escaping strings in the querystring
		env.add_callback("urlencode", 1, [&curl](inja::Arguments &args) {
			std::string input = args.at(0)->get<std::string>();
			char *escaped = curl_easy_escape(curl, input.c_str(), 0);
			input = std::string(escaped);
			curl_free(escaped);
			return input;
		});

		json["seq"] = request_data->sequence_number;

		// Replace the {input} placeholder in the querystring as well
		std::string url = request_data->url;
		try {
			url = env.render(url, json);
		} catch (std::exception &e) {
			obs_log(LOG_WARNING, "Failed to render URL template: %s", e.what());
		}
		response.request_url = url;

		// validate the url
		if (!hasOnlyValidURLCharacters(url)) {
			obs_log(LOG_INFO, "URL '%s' is invalid", url.c_str());
			// Return an error response
			response.error_message = "URL is invalid";
			response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
			return response;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

		// this is needed here, out of the `if` scope below
		std::string request_body_allocated;

		if (request_data->method == "POST") {
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			try {
				request_body_allocated = env.render(request_data->body, json);
			} catch (std::exception &e) {
				obs_log(LOG_WARNING, "Failed to render Body template: %s",
					e.what());
			}
			response.request_body = request_body_allocated;
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body_allocated.c_str());
		} else if (request_data->method == "GET") {
			curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
		}

		// SSL options
		if (request_data->ssl_client_cert_file != "") {
			curl_easy_setopt(curl, CURLOPT_SSLCERT,
					 request_data->ssl_client_cert_file.c_str());
		}
		if (request_data->ssl_client_key_file != "") {
			curl_easy_setopt(curl, CURLOPT_SSLKEY,
					 request_data->ssl_client_key_file.c_str());
		}
		if (request_data->ssl_client_key_pass != "") {
			curl_easy_setopt(curl, CURLOPT_SSLKEYPASSWD,
					 request_data->ssl_client_key_pass.c_str());
		}
		if (!request_data->ssl_verify_peer) {
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		}

		std::map<std::string, std::string> headers;
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);

		// Send the request
		CURLcode code = curl_easy_perform(curl);
		long http_code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		curl_easy_cleanup(curl);

		response.body = responseBody;
		response.body_bytes = responseBodyUint8;
		response.headers = headers;
		response.http_status_code = http_code;

		if (code != CURLE_OK) {
			obs_log(LOG_WARNING, "Failed to send request to '%s': %s", url.c_str(),
				curl_easy_strerror(code));
			if (responseBody.size() > 0) {
				obs_log(LOG_WARNING, "Response body: %s", responseBody.c_str());
			}
			// Return a formatted error response with the message and the HTTP status code
			response.error_message = std::string(curl_easy_strerror(code)) + " (" +
						 std::to_string(http_code) + ")";

			response.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
			return response;
		}

		response.status_code = URL_SOURCE_REQUEST_SUCCESS;
	}

	// Parse the response
	if (request_data->output_type == "JSON") {
		if (request_data->output_json_path != "") {
			response = parse_json_path(response, request_data);
		} else if (request_data->output_json_pointer != "") {
			response = parse_json_pointer(response, request_data);
		} else {
			// attempt to parse as json and return the whole object
			response = parse_json(response, request_data);
		}
	} else if (request_data->output_type == "Key-Value") {
		response = parse_key_value(response, request_data);
	} else if (request_data->output_type == "XML (XPath)") {
		response = parse_xml(response, request_data);
	} else if (request_data->output_type == "XML (XQuery)") {
		response = parse_xml_by_xquery(response, request_data);
	} else if (request_data->output_type == "HTML") {
		response = parse_html(response, request_data);
	} else if (request_data->output_type == "Text") {
		response = parse_regex(response, request_data);
	} else if (request_data->output_type == "Image (data)") {
		response = parse_image_data(response, request_data);
	} else if (request_data->output_type == "Audio (data)") {
		response = parse_audio_data(response, request_data);
	} else {
		obs_log(LOG_INFO, "Invalid output type");
		// Return an error response
		struct request_data_handler_response responseFail;
		responseFail.error_message = "Invalid output type";
		responseFail.status_code = URL_SOURCE_REQUEST_STANDARD_ERROR_CODE;
		return responseFail;
	}

	// If output regex is set - use it to format the output in response.body_parts_parsed
	if (!request_data->post_process_regex.empty()) {
		try {
			std::regex regex(request_data->post_process_regex);
			// for each part of the response body - apply the regex
			for (size_t i = 0; i < response.body_parts_parsed.size(); i++) {
				if (request_data->post_process_regex_is_replace) {
					// replace the whole string with the regex replace string
					response.body_parts_parsed[i] = std::regex_replace(
						response.body_parts_parsed[i], regex,
						request_data->post_process_regex_replace);
				} else {
					std::smatch match;
					if (std::regex_search(response.body_parts_parsed[i], match,
							      regex)) {
						if (match.size() > 1) {
							// replace the whole string with the first capture group
							response.body_parts_parsed[i] = match[1];
						}
					}
				}
			}
		} catch (std::regex_error &e) {
			obs_log(LOG_ERROR, "Failed to parse output_regex: %s", e.what());
		}
	}

	// Return the response
	return response;
}

std::string serialize_request_data(url_source_request_data *request_data)
{
	// Serialize the request data to a string using JSON
	nlohmann::json json;
	json["source_name"] = request_data->source_name;
	json["url"] = request_data->url;
	json["url_or_file"] = request_data->url_or_file;
	json["method"] = request_data->method;
	json["fail_on_http_error"] = request_data->fail_on_http_error;
	json["body"] = request_data->body;
	json["inputs"] = serialize_input_mapping_data(request_data->inputs);
	// SSL options
	json["ssl_client_cert_file"] = request_data->ssl_client_cert_file;
	json["ssl_client_key_file"] = request_data->ssl_client_key_file;
	json["ssl_client_key_pass"] = request_data->ssl_client_key_pass;
	// Output parsing options
	json["output_type"] = request_data->output_type;
	json["output_json_path"] = request_data->output_json_path;
	json["output_json_pointer"] = request_data->output_json_pointer;
	json["output_xpath"] = request_data->output_xpath;
	json["output_xquery"] = request_data->output_xquery;
	json["output_regex"] = request_data->output_regex;
	json["output_regex_flags"] = request_data->output_regex_flags;
	json["output_regex_group"] = request_data->output_regex_group;
	json["output_cssselector"] = request_data->output_cssselector;
	json["kv_delimiter"] = request_data->kv_delimiter;
	// postprocess options
	json["post_process_regex"] = request_data->post_process_regex;
	json["post_process_regex_is_replace"] = request_data->post_process_regex_is_replace;
	json["post_process_regex_replace"] = request_data->post_process_regex_replace;

	nlohmann::json headers_json;
	for (auto header : request_data->headers) {
		headers_json[header.first] = header.second;
	}
	json["headers"] = headers_json;

	return json.dump();
}

url_source_request_data unserialize_request_data(std::string serialized_request_data)
{
	// Unserialize the request data from a string using JSON
	// throughout this function we use .contains() to check if a key exists to avoid
	// exceptions
	url_source_request_data request_data;
	try {
		nlohmann::json json;
		json = nlohmann::json::parse(serialized_request_data);

		request_data.source_name = json.value("source_name", "");
		request_data.url = json["url"].get<std::string>();
		request_data.url_or_file = json.value("url_or_file", "url");
		request_data.method = json["method"].get<std::string>();
		request_data.fail_on_http_error = json.value("fail_on_http_error", false);
		request_data.body = json["body"].get<std::string>();
		request_data.inputs = deserialize_input_mapping_data(json["inputs"].dump());

		// SSL options
		request_data.ssl_client_cert_file = json.value("ssl_client_cert_file", "");
		request_data.ssl_client_key_file = json.value("ssl_client_key_file", "");
		request_data.ssl_client_key_pass = json.value("ssl_client_key_pass", "");

		// Output parsing options
		request_data.output_type = json.value("output_type", "Text");
		request_data.output_json_path = json.value("output_json_path", "");
		if (request_data.output_json_path != "" &&
		    request_data.output_json_path[0] == '/') {
			obs_log(LOG_WARNING,
				"JSON pointer detected in JSON Path. Translating to JSON "
				"path.");
			// this is a json pointer - translate by adding a "$" prefix and replacing all
			// "/"s with "."
			request_data.output_json_path =
				"$." + request_data.output_json_path.substr(1);
			std::replace(request_data.output_json_path.begin(),
				     request_data.output_json_path.end(), '/', '.');
		}
		request_data.output_json_pointer = json.value("output_json_pointer", "");
		if (request_data.output_json_pointer != "" &&
		    request_data.output_json_pointer[0] == '$') {
			obs_log(LOG_WARNING,
				"JSON path detected in JSON Pointer. Translating to JSON "
				"pointer.");
			// this is a json path - translate by replacing all "."s with "/"s
			std::replace(request_data.output_json_pointer.begin(),
				     request_data.output_json_pointer.end(), '.', '/');
			request_data.output_json_pointer = "/" + request_data.output_json_pointer;
		}
		request_data.output_xpath = json["output_xpath"].get<std::string>();
		request_data.output_xquery = json["output_xquery"].get<std::string>();
		request_data.output_regex = json["output_regex"].get<std::string>();
		request_data.output_regex_flags = json["output_regex_flags"].get<std::string>();
		request_data.output_regex_group = json["output_regex_group"].get<std::string>();
		request_data.output_cssselector = json.value("output_cssselector", "");
		request_data.kv_delimiter = json.value("kv_delimiter", "=");

		// postprocess options
		request_data.post_process_regex = json.value("post_process_regex", "");
		request_data.post_process_regex_is_replace =
			json.value("post_process_regex_is_replace", false);
		request_data.post_process_regex_replace =
			json.value("post_process_regex_replace", "");

		nlohmann::json headers_json = json["headers"];
		for (auto header : headers_json.items()) {
			request_data.headers.push_back(
				std::make_pair(header.key(), header.value().get<std::string>()));
		}

	} catch (const std::exception &e) {
		obs_log(LOG_WARNING,
			"Failed to parse JSON request data. Saved request "
			"properties are reset. Error: %s",
			e.what());
		// Return an empty request data object
		return request_data;
	}

	return request_data;
}

bool isURL(const std::string &str)
{
	// List of common URL schemes
	std::string schemes[] = {"http://", "https://"};
	for (const auto &scheme : schemes) {
		if (str.substr(0, scheme.size()) == scheme) {
			return true;
		}
	}
	return false;
}

// Fetch image from url and get bytes
std::vector<uint8_t> fetch_image(std::string url, std::string &mime_type)
{
	// Check if the "url" is actually a file path
	if (isURL(url) == false) {
		// This is a file request (at least it's not a url)

#ifdef _WIN32
		// Convert UTF-8 string to wide string
		int size_needed =
			MultiByteToWideChar(CP_UTF8, 0, &url[0], (int)url.size(), NULL, 0);
		std::wstring wurl(size_needed, 0);
		MultiByteToWideChar(CP_UTF8, 0, &url[0], (int)url.size(), &wurl[0], size_needed);
#else
		std::string wurl = url;
#endif

		// Read the file
		std::ifstream imagefile(wurl, std::ios::binary);
		if (!imagefile) {
			obs_log(LOG_INFO, "Failed to open file, %s", strerror(errno));
			// Return an error response
			std::vector<uint8_t> responseFail;
			return responseFail;
		}
		std::vector<uint8_t> responseBody((std::istreambuf_iterator<char>(imagefile)),
						  std::istreambuf_iterator<char>());
		imagefile.close();

		// infer the mime type from the file extension
		std::string extension = url.substr(url.find_last_of(".") + 1);
		if (extension == "jpg" || extension == "jpeg") {
			mime_type = "image/jpeg";
		} else if (extension == "png") {
			mime_type = "image/png";
		} else if (extension == "gif") {
			mime_type = "image/gif";
		} else {
			mime_type = "image/unknown";
		}

		return responseBody;
	}

	// Build the request with libcurl
	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_WARNING, "Failed to initialize curl");
		// Return an error response
		std::vector<uint8_t> responseFail;
		return responseFail;
	}
	CURLcode code;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFunctionUint8Vector);

	std::vector<uint8_t> responseBody;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);

	// Send the request
	code = curl_easy_perform(curl);
	if (code != CURLE_OK) {
		curl_easy_cleanup(curl);
		obs_log(LOG_INFO, "Failed to send request: %s", curl_easy_strerror(code));
		// Return an error response
		std::vector<uint8_t> responseFail;
		return responseFail;
	}

	// get the mime type from the response headers
	char *ct;
	code = curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
	if (code != CURLE_OK) {
		curl_easy_cleanup(curl);
		obs_log(LOG_INFO, "Failed to get mime type: %s", curl_easy_strerror(code));
		// Return an error response
		std::vector<uint8_t> responseFail;
		return responseFail;
	}
	mime_type = std::string(ct);

	curl_easy_cleanup(curl);

	return responseBody;
}

// BASE64_CHARS
static const std::string BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					"abcdefghijklmnopqrstuvwxyz"
					"0123456789+/";

// encode bytes to base64
std::string base64_encode(const std::vector<uint8_t> &bytes)
{
	std::string encoded;
	encoded.resize(((bytes.size() + 2) / 3) * 4);
	auto it = encoded.begin();
	for (size_t i = 0; i < bytes.size(); i += 3) {
		size_t group = 0;
		group |= (size_t)bytes[i] << 16;
		if (i + 1 < bytes.size()) {
			group |= (size_t)bytes[i + 1] << 8;
		}
		if (i + 2 < bytes.size()) {
			group |= (size_t)bytes[i + 2];
		}
		for (size_t j = 0; j < 4; j++) {
			if (i * 8 / 6 + j < encoded.size()) {
				*it++ = BASE64_CHARS[(group >> 6 * (3 - j)) & 0x3f];
			} else {
				*it++ = '=';
			}
		}
	}
	return encoded;
}
