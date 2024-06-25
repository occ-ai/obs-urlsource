
#include "url-source-callbacks.h"
#include "url-source-data.h"
#include "obs-source-util.h"
#include "plugin-support.h"

#include <obs-module.h>

#include <mutex>
#include "ui/text-render-helper.h"
#include <obs-frontend-api.h>

void acquire_output_source_ref_by_name(const char *output_source_name, obs_source_t **output_source)
{
	if (!is_valid_output_source_name(output_source_name)) {
		obs_log(LOG_ERROR, "Output Source Name '%s' is invalid", output_source_name);
		// text source is not selected
		return;
	}

	// acquire a weak ref to the new text source
	obs_source_t *source = obs_get_source_by_name(output_source_name);
	if (source) {
		*output_source = source;
	} else {
		obs_log(LOG_ERROR, "Source '%s' not found", output_source_name);
	}
}

void setTextCallback(const std::string &str, const output_mapping &mapping)
{
	obs_source_t *target;
	acquire_output_source_ref_by_name(mapping.output_source.c_str(), &target);
	if (!target) {
		obs_log(LOG_ERROR, "Source target is null");
		return;
	}

	auto target_settings = obs_source_get_settings(target);

	if (strcmp(obs_source_get_id(target), "ffmpeg_source") == 0) {
		// if the target source is a media source - set the input field to the text and disable the local file
		obs_data_set_bool(target_settings, "is_local_file", false);
		obs_data_set_bool(target_settings, "clear_on_media_end", true);
		obs_data_set_string(target_settings, "local_file", "");
		obs_data_set_string(target_settings, "input", str.c_str());
		obs_data_set_bool(target_settings, "looping", false);
	} else {
		// if the target source is a text source - set the text field
		obs_data_set_string(target_settings, "text", str.c_str());
	}
	obs_source_update(target, target_settings);
	if (mapping.unhide_output_source) {
		// unhide the output source
		obs_source_set_enabled(target, true);
		obs_enum_scenes(
			[](void *target_ptr, obs_source_t *scene_source) -> bool {
				obs_scene_t *scene = obs_scene_from_source(scene_source);
				if (scene == nullptr) {
					return true;
				}
				obs_sceneitem_t *scene_item = obs_scene_sceneitem_from_source(
					scene, (obs_source_t *)target_ptr);
				if (scene_item == nullptr) {
					return true;
				}
				obs_sceneitem_set_visible(scene_item, true);
				return false; // stop enumerating
			},
			target);
	}
	obs_data_release(target_settings);
	obs_source_release(target);
};

void setAudioCallback(const std::string &str, const output_mapping &mapping)
{
	obs_source_t *target;
	acquire_output_source_ref_by_name(mapping.output_source.c_str(), &target);
	if (!target) {
		obs_log(LOG_ERROR, "Output source target is null");
		return;
	}
	// assert the source is a media source
	if (strcmp(obs_source_get_id(target), "ffmpeg_source") != 0) {
		obs_source_release(target);
		obs_log(LOG_ERROR, "Output source is not a media source");
		return;
	}
	obs_data_t *media_settings = obs_source_get_settings(target);
	obs_data_set_bool(media_settings, "is_local_file", true);
	obs_data_set_bool(media_settings, "clear_on_media_end", true);
	obs_data_set_string(media_settings, "local_file", str.c_str());
	obs_data_set_bool(media_settings, "looping", false);
	obs_source_update(target, media_settings);
	obs_data_release(media_settings);

	obs_source_media_restart(target);
	obs_source_release(target);
};

std::string renderOutputTemplate(inja::Environment &env, const std::string &input,
				 const request_data_handler_response &response)
try {
	// Use Inja to render the template
	nlohmann::json data;
	if (response.body_parts_parsed.size() > 1) {
		for (size_t i = 0; i < response.body_parts_parsed.size(); i++) {
			data["output" + std::to_string(i)] = response.body_parts_parsed[i];
		}
		// in "output" add an array of all the outputs
		data["output"] = response.body_parts_parsed;
	} else {
		data["output"] = response.body_parts_parsed[0];
	}
	if (response.key_value_pairs.size() > 0) {
		for (const auto &pair : response.key_value_pairs) {
			data[pair.first] = pair.second;
		}
	}
	data["body"] = response.body_json;
	return env.render(input, data);
} catch (std::exception &e) {
	obs_log(LOG_ERROR, "Failed to parse template: %s", e.what());
	return "";
}

void render_internal(const std::string &text, struct url_source_data *usd,
		     const output_mapping &mapping)
{
	uint8_t *renderBuffer = nullptr;
	uint32_t width = usd->render_width;
	uint32_t height = 0;

	// render the text with QTextDocument
	render_text_with_qtextdocument(text, width, height, &renderBuffer, mapping.css_props);
	// Update the frame
	usd->frame.data[0] = renderBuffer;
	usd->frame.linesize[0] = width * 4;
	usd->frame.width = width;
	usd->frame.height = height;

	// Send the frame
	obs_source_output_video(usd->source, &usd->frame);

	bfree(usd->frame.data[0]);
	usd->frame.data[0] = nullptr;
}

std::string prepare_text_from_template(const output_mapping &mapping,
				       const request_data_handler_response &response,
				       const url_source_request_data &request,
				       bool output_is_image_url)
{
	// prepare the text from the template
	std::string text = mapping.template_string;
	// if the template is empty use the response body
	if (text.empty()) {
		return response.body_parts_parsed[0];
	}

	inja::Environment env;

	// if output is image or image-URL - fetch the image and convert it to base64
	if (output_is_image_url || request.output_type == "Image (data)") {
		std::vector<uint8_t> image_data;
		std::string mime_type = "image/png";
		if (request.output_type == "Image (data)") {
			// if the output type is image data - use the response body bytes
			image_data = response.body_bytes;
			// get the mime type from the response headers if available
			if (response.headers.find("content-type") != response.headers.end()) {
				mime_type = response.headers.at("content-type");
			}
		} else {
			text = renderOutputTemplate(env, text, response);
			// use fetch_image to get the image
			image_data = fetch_image(text.c_str(), mime_type);
		}
		// convert the image to base64
		const std::string base64_image = base64_encode(image_data);
		// build an image tag with the base64 image
		text = "<img src=\"data:" + mime_type + ";base64," + base64_image + "\" />";
	} else {
		text = renderOutputTemplate(env, text, response);
	}
	return text;
}

void output_with_mapping(const request_data_handler_response &response, struct url_source_data *usd)
{
	std::vector<output_mapping> mappings;

	{
		// lock the mapping mutex to get a local copy of the mappings
		std::unique_lock<std::mutex> lock(usd->output_mapping_mutex);
		mappings = usd->output_mapping_data.mappings;
	}

	// if there are no mappings - log
	if (mappings.empty()) {
		// if there are no mappings - log
		obs_log(LOG_WARNING, "No mappings found");
		return;
	}

	bool any_internal_rendering = false;
	// iterate over the mappings and output the text with each one
	for (const auto &mapping : mappings) {
		if (usd->request_data.output_type == "Audio (data)") {
			if (!is_valid_output_source_name(mapping.output_source.c_str())) {
				obs_log(LOG_ERROR, "Must select an output source for audio output");
			} else {
				setAudioCallback(response.body, mapping);
			}
			continue;
		}

		std::string text = prepare_text_from_template(mapping, response, usd->request_data,
							      usd->output_is_image_url);

		if (usd->send_to_stream && !usd->output_is_image_url) {
			// Send the output to the current stream as caption, if it's not an image and a stream is open
			obs_output_t *streaming_output = obs_frontend_get_streaming_output();
			if (streaming_output) {
				obs_output_output_caption_text1(streaming_output, text.c_str());
				obs_output_release(streaming_output);
			}
		}

		if (is_valid_output_source_name(mapping.output_source.c_str()) &&
		    mapping.output_source != none_internal_rendering) {
			// If an output source is selected - use it for rendering
			setTextCallback(text, mapping);
		} else {
			any_internal_rendering = true;
			render_internal(text, usd, mapping);
		} // end if not text source
	}

	if (!any_internal_rendering) {
		// Send a null frame to hide the source
		obs_source_output_video(usd->source, nullptr);
	}
}
