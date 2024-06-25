/*
URL Source
Copyright (C) 2023 Roy Shilkrot roy.shil@gmail.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "ui/RequestBuilder.h"
#include "ui/text-render-helper.h"
#include "ui/outputmapping.h"
#include "request-data.h"
#include "plugin-support.h"
#include "url-source.h"
#include "url-source-data.h"
#include "url-source-thread.h"
#include "url-source-callbacks.h"
#include "obs-source-util.h"
#include "mapping-data.h"

#include <stdlib.h>
#include <graphics/graphics.h>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <inja/inja.hpp>

#include <thread>
#include <mutex>
#include <chrono>
#include <memory>
#include <regex>

url_source_data::url_source_data() : output_mapping_mutex(), curl_mutex(), curl_thread_cv() {}

const char *url_source_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "URL Source";
}

void url_source_destroy(void *data)
{
	struct url_source_data *usd = reinterpret_cast<struct url_source_data *>(data);

	stop_and_join_curl_thread(usd);

	if (usd->frame.data[0] != nullptr) {
		bfree(usd->frame.data[0]);
		usd->frame.data[0] = nullptr;
	}

	bfree(usd);
}

void save_request_info_on_settings(obs_data_t *settings,
				   struct url_source_request_data *request_data)
{
	// serialize request data
	std::string serialized_request_data = serialize_request_data(request_data);
	// Save on settings
	obs_data_set_string(settings, "request_data", serialized_request_data.c_str());
	// Update the URL input string
	obs_data_set_string(settings, "url", request_data->url.c_str());
}

void *url_source_create(obs_data_t *settings, obs_source_t *source)
{
	void *p = bzalloc(sizeof(struct url_source_data));
	struct url_source_data *usd = new (p) url_source_data();
	usd->source = source;
	usd->request_data = url_source_request_data();

	usd->frame.data[0] = nullptr;
	usd->frame.format = VIDEO_FORMAT_BGRA;

	// get request data from settings
	std::string serialized_request_data = obs_data_get_string(settings, "request_data");
	if (serialized_request_data.empty()) {
		// Default request data
		usd->request_data.url = std::string("https://catfact.ninja/fact");
		usd->request_data.url_or_file = std::string("url");
		usd->request_data.method = std::string("GET");
		usd->request_data.output_type = std::string("json");
		usd->request_data.output_json_path = std::string("/fact");

		save_request_info_on_settings(settings, &(usd->request_data));
	} else {
		// Unserialize request data
		usd->request_data = unserialize_request_data(serialized_request_data);
	}

	usd->output_mapping_data = deserialize_output_mapping_data(
		obs_data_get_string(settings, "output_mapping_data"));
	usd->request_data.source_name = std::string(obs_source_get_name(source));
	usd->update_timer_ms = (uint32_t)obs_data_get_int(settings, "update_timer");
	usd->run_while_not_visible = obs_data_get_bool(settings, "run_while_not_visible");
	usd->output_is_image_url = obs_data_get_bool(settings, "is_image_url");
	usd->send_to_stream = obs_data_get_bool(settings, "send_to_stream");

	if (obs_source_active(source) && obs_source_showing(source)) {
		// start the thread
		usd->curl_thread_run = true;
		std::thread new_curl_thread(curl_loop, usd);
		usd->curl_thread.swap(new_curl_thread);
	} else {
		// thread should not be running
		usd->curl_thread_run = false;
	}

	return usd;
}

void url_source_update(void *data, obs_data_t *settings)
{
	struct url_source_data *usd = reinterpret_cast<struct url_source_data *>(data);
	// Update the request data from the settings
	usd->update_timer_ms = (uint32_t)obs_data_get_int(settings, "update_timer");
	usd->run_while_not_visible = obs_data_get_bool(settings, "run_while_not_visible");
	usd->output_is_image_url = obs_data_get_bool(settings, "is_image_url");
	usd->send_to_stream = obs_data_get_bool(settings, "send_to_stream");
	usd->render_width = (uint32_t)obs_data_get_int(settings, "render_width");
	usd->output_mapping_data = deserialize_output_mapping_data(
		obs_data_get_string(settings, "output_mapping_data"));
	usd->request_data = unserialize_request_data(obs_data_get_string(settings, "request_data"));
}

void url_source_defaults(obs_data_t *s)
{
	// Default request data
	struct url_source_request_data request_data;
	request_data.url = "https://catfact.ninja/fact";
	request_data.url_or_file = "url";
	request_data.method = "GET";
	request_data.output_type = "JSON";
	request_data.output_json_path = "/fact";

	// serialize request data
	std::string serialized_request_data = serialize_request_data(&request_data);
	obs_data_set_default_string(s, "request_data", serialized_request_data.c_str());
	obs_data_set_default_string(s, "url", request_data.url.c_str());

	// serialize mapping data
	struct output_mapping_data mapping_data;
	mapping_data.mappings.push_back(
		{"output", "None / Internal rendering", "{{output}}",
		 "background-color: transparent;\ncolor: #FFFFFF;\nfont-size: 48px;"});
	std::string serialized_mapping_data = serialize_output_mapping_data(mapping_data);
	obs_data_set_default_string(s, "output_mapping_data", serialized_mapping_data.c_str());

	// Default output source
	obs_data_set_default_string(s, "text_sources", "none");
	// obs_data_set_default_bool(s, "unhide_output_source", false);

	obs_data_set_default_bool(s, "send_to_stream", false);

	// Default update timer setting in milliseconds
	obs_data_set_default_int(s, "update_timer", 1000);

	obs_data_set_default_bool(s, "run_while_not_visible", false);

	// Is Image URL default false
	obs_data_set_default_bool(s, "is_image_url", false);

	// Default CSS properties
	obs_data_set_default_string(
		s, "css_props",
		"background-color: transparent;\ncolor: #FFFFFF;\nfont-size: 48px;");

	// Default Template
	obs_data_set_default_string(s, "template", "{{output}}");

	// Default Render Width
	obs_data_set_default_int(s, "render_width", 640);
}

bool setup_request_button_click(obs_properties_t *, obs_property_t *, void *button_data)
{
	struct url_source_data *button_usd =
		reinterpret_cast<struct url_source_data *>(button_data);
	// Open the Request Builder dialog
	std::unique_ptr<RequestBuilder> builder(new RequestBuilder(
		&(button_usd->request_data),
		[button_usd]() {
			// Update the request data from the settings
			obs_data_t *settings = obs_source_get_settings(button_usd->source);
			save_request_info_on_settings(settings, &(button_usd->request_data));
		},
		(QWidget *)obs_frontend_get_main_window()));
	builder->exec();
	return true;
}

bool output_mapping_and_template_button_click(obs_properties_t *, obs_property_t *,
					      void *button_data)
{
	struct url_source_data *button_usd =
		reinterpret_cast<struct url_source_data *>(button_data);
	// Open the Output Mapping dialog
	std::unique_ptr<OutputMapping> output_mapping(new OutputMapping(
		button_usd->output_mapping_data,
		[button_usd](const output_mapping_data &new_mapping_data) {
			if (button_usd->source == nullptr) {
				obs_log(LOG_ERROR, "Source is null");
				return;
			}

			// Update the output mapping data from the settings
			obs_data_t *settings = obs_source_get_settings(button_usd->source);
			if (settings == nullptr) {
				obs_log(LOG_ERROR, "Failed to get settings for source");
				return;
			}

			std::string serialized_mapping_data =
				serialize_output_mapping_data(new_mapping_data);
			obs_data_set_string(settings, "output_mapping_data",
					    serialized_mapping_data.c_str());

			{
				// lock the mapping data mutex
				std::unique_lock<std::mutex> lock(button_usd->output_mapping_mutex);
				button_usd->output_mapping_data = new_mapping_data;
			}

			obs_source_update(button_usd->source, settings);
			obs_data_release(settings);
		},
		(QWidget *)obs_frontend_get_main_window()));
	output_mapping->exec();
	return true;
}

obs_properties_t *url_source_properties(void *data)
{
	struct url_source_data *usd = reinterpret_cast<struct url_source_data *>(data);

	obs_properties_t *ppts = obs_properties_create();
	// URL input string
	obs_property_t *urlprop =
		obs_properties_add_text(ppts, "url", "URL / File", OBS_TEXT_DEFAULT);
	// Disable the URL input since it's setup via the Request Builder dialog
	obs_property_set_enabled(urlprop, false);

	// Add button to open the Request Builder dialog
	obs_properties_add_button2(ppts, "setup_request_button", "Setup Data Source",
				   setup_request_button_click, usd);

	obs_properties_add_button2(ppts, "output_mapping_and_template",
				   "Setup Outputs and Templates",
				   output_mapping_and_template_button_click, usd);

	// Update timer setting in milliseconds
	obs_properties_add_int(ppts, "update_timer", "Update Timer (ms)", 100, 1000000, 100);

	// Run timer while not visible
	obs_properties_add_bool(ppts, "run_while_not_visible", "Run while not visible?");

	obs_properties_add_bool(ppts, "send_to_stream",
				"Send output to current stream as captions");

	// Is Image URL boolean checkbox
	obs_properties_add_bool(ppts, "is_image_url", "Output is image URL (fetch and show image)");

	obs_properties_add_int(ppts, "render_width", "Render Width (px)", 100, 10000, 1);

	// Add a informative text about the plugin
	obs_properties_add_text(
		ppts, "info",
		QString(PLUGIN_INFO_TEMPLATE).arg(PLUGIN_VERSION).toStdString().c_str(),
		OBS_TEXT_INFO);

	return ppts;
}

void url_source_activate(void *data)
{
	if (data == nullptr) {
		return;
	}
	struct url_source_data *usd = reinterpret_cast<struct url_source_data *>(data);
	if (usd == nullptr) {
		return;
	}
	if (usd->curl_thread_run) {
		// Thread is already running
		return;
	}
	usd->curl_thread_run = true;
	std::thread new_curl_thread(curl_loop, usd);
	usd->curl_thread.swap(new_curl_thread);
}

void url_source_deactivate(void *data)
{
	struct url_source_data *usd = reinterpret_cast<struct url_source_data *>(data);
	if (!usd->run_while_not_visible) {
		// Stop the thread
		stop_and_join_curl_thread(usd);
	}
}
