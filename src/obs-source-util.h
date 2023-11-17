#ifndef OBS_SOURCE_UTIL_H
#define OBS_SOURCE_UTIL_H

#include <obs.h>

#include <string>
#include <vector>

inline bool is_obs_source_text(obs_source_t *source)
{
	const auto source_id = obs_source_get_id(source);
	return strcmp(source_id, "text_ft2_source_v2") == 0 ||
	       strcmp(source_id, "text_gdiplus_v2") == 0;
}

inline bool is_obs_source_text(const std::string &source_name)
{
	obs_source_t *source = obs_get_source_by_name(source_name.c_str());
	const bool is_text = is_obs_source_text(source);
	obs_source_release(source);
	return is_text;
}

struct source_render_data {
	gs_texrender_t *texrender;
	gs_stagesurf_t *stagesurface;
};

void init_source_render_data(source_render_data *tf);
void destroy_source_render_data(source_render_data *tf);

std::vector<uint8_t> get_rgba_from_source_render(obs_source_t *source, source_render_data *tf,
						 uint32_t &width, uint32_t &height);

std::string convert_rgba_buffer_to_png_base64(const std::vector<uint8_t> &rgba, uint32_t width,
					      uint32_t height);

#endif // OBS_SOURCE_UTIL_H