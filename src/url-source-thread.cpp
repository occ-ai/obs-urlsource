
#include "url-source-thread.h"
#include "url-source-callbacks.h"
#include "request-data.h"
#include "plugin-support.h"
#include "obs-source-util.h"
#include "ui/text-render-helper.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-source.h>
#include <inja/inja.hpp>

void curl_loop(struct url_source_data *usd)
{
	obs_log(LOG_INFO, "Starting URL Source thread, update timer: %d", usd->update_timer_ms);

	inja::Environment env;

	while (usd->curl_thread_run) {
		// time the request
		uint64_t request_start_time_ns = get_time_ns();

		// Send the request
		struct request_data_handler_response response =
			request_data_handler(&(usd->request_data));
		if (response.status_code != URL_SOURCE_REQUEST_SUCCESS) {
			if (response.status_code != URL_SOURCE_REQUEST_BENIGN_ERROR_CODE) {
				obs_log(LOG_INFO, "Failed to send request: %s",
					response.error_message.c_str());
			}
		} else {
			if (response.body_parts_parsed.empty()) {
				response.body_parts_parsed.push_back(response.body);
			}

			output_with_mapping(response, usd);
		}

		// time the request, calculate the remaining time and sleep
		const uint64_t request_end_time_ns = get_time_ns();
		const uint64_t request_time_ns = request_end_time_ns - request_start_time_ns;
		const int64_t sleep_time_ms =
			(int64_t)(usd->update_timer_ms) - (int64_t)(request_time_ns / 1000000);
		if (sleep_time_ms > 0) {
			std::unique_lock<std::mutex> lock(usd->curl_mutex);
			// Sleep for n ns as per the update timer for the remaining time
			usd->curl_thread_cv.wait_for(lock,
						     std::chrono::milliseconds(sleep_time_ms));
		}
	}
	obs_log(LOG_INFO, "Stopping URL Source thread");
}

void stop_and_join_curl_thread(struct url_source_data *usd)
{
	if (!usd->curl_thread_run) {
		// Thread is already stopped
		return;
	}
	usd->curl_thread_run = false;
	usd->curl_thread_cv.notify_all();
	if (usd->curl_thread.joinable()) {
		usd->curl_thread.join();
	}
}
