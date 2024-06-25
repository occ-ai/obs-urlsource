#ifndef URL_SOURCE_DATA_H
#define URL_SOURCE_DATA_H

#include "request-data.h"
#include "mapping-data.h"

#include <obs-module.h>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

struct url_source_data {
	obs_source_t *source = nullptr;
	struct url_source_request_data request_data;
	struct request_data_handler_response response;
	struct output_mapping_data output_mapping_data;
	uint32_t update_timer_ms = 1000;
	bool run_while_not_visible = false;
	bool output_is_image_url = false;
	struct obs_source_frame frame;
	bool send_to_stream = false;
	uint32_t render_width = 640;

	std::mutex output_mapping_mutex;
	std::mutex curl_mutex;
	std::thread curl_thread;
	std::condition_variable curl_thread_cv;
	std::atomic<bool> curl_thread_run = false;

	// ctor must initialize mutex
	explicit url_source_data();
};

#endif
