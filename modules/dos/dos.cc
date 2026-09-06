// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Zorin

#include "dos.h"
#include "runtime.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#include "ymfm/src/ymfm_opl.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#define SOKOL_GLCORE
#include "sokol/sokol_app.h"
#include "sokol/sokol_audio.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_glue.h"
#include "sokol/sokol_log.h"

namespace
{

	constexpr int WINDOW_SCALE{3};
	constexpr double PIXEL_ASPECT{1.2};
	constexpr size_t PALETTE_COLORS{256};

	constexpr char VERTEX_SOURCE[]{
		R"(
#version 410
in vec2 pos;
out vec2 uv;
void main() {
  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
  uv = vec2(pos.x, 1.0 - pos.y);
}
)"};

	constexpr char PLAIN_FRAGMENT_SOURCE[]{
		R"(
#version 410
uniform sampler2D indices;
uniform sampler2D palette;
in vec2 uv;
out vec4 color;
void main() {
  float index = texture(indices, uv).r;
  color = texture(palette, vec2(index * (255.0 / 256.0) + (0.5 / 256.0), 0.5));
}
)"};

	constexpr char CRT_FRAGMENT_SOURCE[]{
		R"(
#version 410
uniform sampler2D indices;
uniform sampler2D palette;
in vec2 uv;
out vec4 color;
const float CURVATURE = 0.02;
const float OVERSCAN = 1.0 + 2.0 * CURVATURE;
const float BASE_BLEED = 0.035;
const float HALATION_NEAR = 0.14;
const float HALATION_FAR = 0.05;
const float SCANLINE_DEPTH = 0.06;
const float MASK_LOW = 0.94;
const float VIGNETTE_DEPTH = 0.16;
const vec3 SHEEN_COLOR = vec3(0.055, 0.07, 0.085);
vec3 screen_color(vec2 sample_uv) {
  float index = texture(indices, sample_uv).r;
  return texture(palette, vec2(index * (255.0 / 256.0) + (0.5 / 256.0), 0.5)).rgb;
}
void main() {
  vec2 centered = uv * 2.0 - 1.0;
  vec2 curved = centered * (1.0 + dot(centered, centered) * CURVATURE) / OVERSCAN;
  vec2 sample_uv = curved * 0.5 + 0.5;
  if (any(lessThan(sample_uv, vec2(0.0))) || any(greaterThan(sample_uv, vec2(1.0)))) {
    color = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
  vec2 texel = 1.0 / vec2(textureSize(indices, 0));
  vec3 center = screen_color(sample_uv);
  vec3 left = screen_color(sample_uv - vec2(texel.x, 0.0));
  vec3 right = screen_color(sample_uv + vec2(texel.x, 0.0));
  vec3 far_left = screen_color(sample_uv - vec2(texel.x * 2.0, 0.0));
  vec3 far_right = screen_color(sample_uv + vec2(texel.x * 2.0, 0.0));
  vec3 luminance = vec3(0.2126, 0.7152, 0.0722);
  float left_glow = smoothstep(0.24, 0.82, dot(left, luminance));
  float right_glow = smoothstep(0.24, 0.82, dot(right, luminance));
  float far_left_glow = smoothstep(0.38, 0.95, dot(far_left, luminance));
  float far_right_glow = smoothstep(0.38, 0.95, dot(far_right, luminance));
  vec3 rgb = center * (1.0 - BASE_BLEED * 2.0) + (left + right) * BASE_BLEED;
  rgb += (left * left_glow + right * right_glow) * HALATION_NEAR;
  rgb += (far_left * far_left_glow + far_right * far_right_glow) * HALATION_FAR;
  float row = sample_uv.y * float(textureSize(indices, 0).y);
  float scanline = 1.0 - SCANLINE_DEPTH + SCANLINE_DEPTH * cos(row * 6.28318530718);
  float mask_phase = mod(floor(gl_FragCoord.x), 3.0);
  vec3 mask = mask_phase < 1.0 ? vec3(1.0, MASK_LOW, MASK_LOW) :
              mask_phase < 2.0 ? vec3(MASK_LOW, 1.0, MASK_LOW) : vec3(MASK_LOW, MASK_LOW, 1.0);
  float vignette = 1.0 - VIGNETTE_DEPTH * smoothstep(0.35, 1.45, dot(centered, centered));
  float sheen = pow(max(0.0, 1.0 - length(centered - vec2(-0.45, -0.65)) / 1.25), 4.0);
  rgb = rgb * scanline * mask * vignette + SHEEN_COLOR * sheen;
  color = vec4(rgb, 1.0);
}
)"};

	enum class ScreenEmulation
	{
		None,
		Crt
	};

	struct Video
	{
		VmState* vm{};
		Atom frame_proc{};
		int width{};
		int height{};
		sg_image indices{};
		sg_image palette{};
		sg_view index_view{};
		sg_view palette_view{};
		sg_sampler sampler{};
		sg_pipeline pipeline{};
		sg_buffer quad{};
		ScreenEmulation screen_emulation{ScreenEmulation::None};
		bool ready{};
		bool palette_dirty{};
		uint8_t rgba[PALETTE_COLORS * 4]{};
		bool keys[SAPP_MAX_KEYCODES]{};
		bool mouse_buttons[3]{};
		double mouse_dx{};
		double mouse_dy{};
	};

	Video video{};

	struct Voice
	{
		std::vector<uint8_t> samples;
		double step{};
		double cursor{};
		bool playing{};
	};

	// The DOS machine drove the Sound Blaster and the PC speaker at once, so they are separate voices.
	constexpr size_t VOICE_DIGI{0};
	constexpr size_t VOICE_PC{1};
	constexpr size_t VOICE_COUNT{2};

	struct YmfmInterface : ymfm::ymfm_interface
	{
	};

	struct Audio
	{
		std::mutex lock;
		Voice voices[VOICE_COUNT];
		YmfmInterface ymfm_interface;
		ymfm::ym3812 adlib{ymfm_interface};
		double adlib_step{};
		double adlib_phase{1.0};
		float adlib_sample{};
		float left{1.0f};
		float right{1.0f};
		bool started{};
	};

	constexpr uint32_t ADLIB_CLOCK{3579545};
	constexpr int MAX_ATTENUATION{15};

	Audio audio{};

	float next_sample(Voice& voice)
	{
		if (!voice.playing)
		{
			return 0.0f;
		}

		size_t index = static_cast<size_t>(voice.cursor);
		if (index >= voice.samples.size())
		{
			voice.playing = false;
			return 0.0f;
		}

		voice.cursor += voice.step;
		return (static_cast<float>(voice.samples[index]) - 128.0f) / 128.0f;
	}

	float next_adlib_sample()
	{
		if (audio.adlib_step == 0.0)
		{
			audio.adlib_step = static_cast<double>(audio.adlib.sample_rate(ADLIB_CLOCK)) /
			                   saudio_sample_rate();
		}

		audio.adlib_phase += audio.adlib_step;
		while (audio.adlib_phase >= 1.0)
		{
			ymfm::ym3812::output_data output{};
			audio.adlib.generate(&output);
			audio.adlib_sample = static_cast<float>(output.data[0]) / 32768.0f;
			audio.adlib_phase -= 1.0;
		}

		return audio.adlib_sample;
	}

	void stream_cb(float* buffer, int num_frames, int num_channels)
	{
		const std::lock_guard<std::mutex> held{audio.lock};
		const float gains[2]{audio.left, audio.right};

		for (int frame{0}; frame < num_frames; frame++)
		{
			float digi = next_sample(audio.voices[VOICE_DIGI]);
			float pc = next_sample(audio.voices[VOICE_PC]);
			float adlib = next_adlib_sample();

			for (int channel{0}; channel < num_channels; channel++)
			{
				float mixed = digi * gains[channel & 1] + pc + adlib;
				buffer[frame * num_channels + channel] = std::clamp(mixed, -1.0f, 1.0f);
			}
		}
	}

	bool start_audio()
	{
		if (!audio.started)
		{
			saudio_desc desc{};
			desc.stream_cb = stream_cb;
			desc.num_channels = 2;
			desc.logger.func = slog_func;
			saudio_setup(&desc);

			audio.started = true;
		}

		return saudio_isvalid();
	}

	struct KeyName
	{
		const char* name;
		sapp_keycode code;
	};

	constexpr KeyName KEY_NAMES[]{
		{"up", SAPP_KEYCODE_UP},
		{"down", SAPP_KEYCODE_DOWN},
		{"left", SAPP_KEYCODE_LEFT},
		{"right", SAPP_KEYCODE_RIGHT},
		{"escape", SAPP_KEYCODE_ESCAPE},
		{"space", SAPP_KEYCODE_SPACE},
		{"enter", SAPP_KEYCODE_ENTER},
		{"tab", SAPP_KEYCODE_TAB},
		{"backspace", SAPP_KEYCODE_BACKSPACE},
		{"control", SAPP_KEYCODE_LEFT_CONTROL},
		{"alt", SAPP_KEYCODE_LEFT_ALT},
		{"shift", SAPP_KEYCODE_LEFT_SHIFT},
		{"right-control", SAPP_KEYCODE_RIGHT_CONTROL},
		{"right-alt", SAPP_KEYCODE_RIGHT_ALT},
		{"right-shift", SAPP_KEYCODE_RIGHT_SHIFT},
		{"caps-lock", SAPP_KEYCODE_CAPS_LOCK},
		{"pause", SAPP_KEYCODE_PAUSE},
		{"insert", SAPP_KEYCODE_INSERT},
		{"delete", SAPP_KEYCODE_DELETE},
		{"home", SAPP_KEYCODE_HOME},
		{"end", SAPP_KEYCODE_END},
		{"page-up", SAPP_KEYCODE_PAGE_UP},
		{"page-down", SAPP_KEYCODE_PAGE_DOWN},
		{"minus", SAPP_KEYCODE_MINUS},
		{"equal", SAPP_KEYCODE_EQUAL},
		{"left-bracket", SAPP_KEYCODE_LEFT_BRACKET},
		{"right-bracket", SAPP_KEYCODE_RIGHT_BRACKET},
		{"semicolon", SAPP_KEYCODE_SEMICOLON},
		{"apostrophe", SAPP_KEYCODE_APOSTROPHE},
		{"grave", SAPP_KEYCODE_GRAVE_ACCENT},
		{"backslash", SAPP_KEYCODE_BACKSLASH},
		{"comma", SAPP_KEYCODE_COMMA},
		{"period", SAPP_KEYCODE_PERIOD},
		{"slash", SAPP_KEYCODE_SLASH},
		{"f1", SAPP_KEYCODE_F1},
		{"f2", SAPP_KEYCODE_F2},
		{"f3", SAPP_KEYCODE_F3},
		{"f4", SAPP_KEYCODE_F4},
		{"f5", SAPP_KEYCODE_F5},
		{"f6", SAPP_KEYCODE_F6},
		{"f7", SAPP_KEYCODE_F7},
		{"f8", SAPP_KEYCODE_F8},
		{"f9", SAPP_KEYCODE_F9},
		{"f10", SAPP_KEYCODE_F10},
		{"f11", SAPP_KEYCODE_F11},
		{"f12", SAPP_KEYCODE_F12}};

	size_t voice_index(VmState& s, Atom channel)
	{
		const std::string& name = *slow_unbox<Symbol>(s, channel);

		if (name == "digi")
		{
			return VOICE_DIGI;
		}
		if (name == "pc")
		{
			return VOICE_PC;
		}

		JET_DIE(&s, "sound channel: unknown channel '{}', expected 'digi or 'pc", name);
	}

	sapp_keycode key_code(VmState& s, Atom key)
	{
		const std::string& name = *slow_unbox<Symbol>(s, key);

		if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z')
		{
			return static_cast<sapp_keycode>(SAPP_KEYCODE_A + (name[0] - 'a'));
		}
		if (name.size() == 1 && name[0] >= '0' && name[0] <= '9')
		{
			return static_cast<sapp_keycode>(SAPP_KEYCODE_0 + (name[0] - '0'));
		}
		for (const KeyName& entry : KEY_NAMES)
		{
			if (name == entry.name)
			{
				return entry.code;
			}
		}

		JET_DIE(&s, "key-down?: unknown key '{}'", name);
	}

	void init_cb()
	{
		sg_desc setup{};
		setup.logger.func = slog_func;
		setup.environment = sglue_environment();
		sg_setup(&setup);

		sg_image_desc index_image{};
		index_image.type = SG_IMAGETYPE_2D;
		index_image.usage.stream_update = true;
		index_image.width = video.width;
		index_image.height = video.height;
		index_image.pixel_format = SG_PIXELFORMAT_R8;
		video.indices = sg_make_image(&index_image);

		sg_image_desc palette_image{};
		palette_image.type = SG_IMAGETYPE_2D;
		palette_image.usage.stream_update = true;
		palette_image.width = static_cast<int>(PALETTE_COLORS);
		palette_image.height = 1;
		palette_image.pixel_format = SG_PIXELFORMAT_RGBA8;
		video.palette = sg_make_image(&palette_image);

		sg_view_desc index_view{};
		index_view.texture.image = video.indices;
		video.index_view = sg_make_view(&index_view);

		sg_view_desc palette_view{};
		palette_view.texture.image = video.palette;
		video.palette_view = sg_make_view(&palette_view);

		sg_sampler_desc sampler{};
		sampler.min_filter = SG_FILTER_NEAREST;
		sampler.mag_filter = SG_FILTER_NEAREST;
		sampler.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
		sampler.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
		video.sampler = sg_make_sampler(&sampler);

		static const float corners[]{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
		sg_buffer_desc quad{};
		quad.size = sizeof(corners);
		quad.usage.vertex_buffer = true;
		quad.usage.immutable = true;
		quad.data.ptr = corners;
		quad.data.size = sizeof(corners);
		video.quad = sg_make_buffer(&quad);

		auto&& make_pipeline = [](const char* fragment_source)
		{
			sg_shader_desc shader{};
			shader.vertex_func.source = VERTEX_SOURCE;
			shader.fragment_func.source = fragment_source;
			shader.attrs[0].base_type = SG_SHADERATTRBASETYPE_FLOAT;
			shader.attrs[0].glsl_name = "pos";
			shader.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
			shader.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
			for (size_t slot{0}; slot < 2; slot++)
			{
				shader.views[slot].texture.stage = SG_SHADERSTAGE_FRAGMENT;
				shader.views[slot].texture.image_type = SG_IMAGETYPE_2D;
				shader.views[slot].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
				shader.texture_sampler_pairs[slot].stage = SG_SHADERSTAGE_FRAGMENT;
				shader.texture_sampler_pairs[slot].view_slot = static_cast<uint8_t>(slot);
				shader.texture_sampler_pairs[slot].sampler_slot = 0;
			}
			shader.texture_sampler_pairs[0].glsl_name = "indices";
			shader.texture_sampler_pairs[1].glsl_name = "palette";

			sg_pipeline_desc pipeline{};
			pipeline.shader = sg_make_shader(&shader);
			pipeline.layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
			pipeline.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
			return sg_make_pipeline(&pipeline);
		};

		const char* fragment_source = video.screen_emulation == ScreenEmulation::Crt
		                              ? CRT_FRAGMENT_SOURCE
		                              : PLAIN_FRAGMENT_SOURCE;
		video.pipeline = make_pipeline(fragment_source);

		video.ready = true;
		video.palette_dirty = true;
	}

	void frame_cb()
	{
		jet_enter_vm(*video.vm, video.frame_proc, nullptr, 0);
	}

	void event_cb(const sapp_event* event)
	{
		if (event->type == SAPP_EVENTTYPE_MOUSE_MOVE)
		{
			video.mouse_dx += event->mouse_dx;
			video.mouse_dy += event->mouse_dy;
			return;
		}
		if (event->type == SAPP_EVENTTYPE_MOUSE_DOWN || event->type == SAPP_EVENTTYPE_MOUSE_UP)
		{
			if (event->mouse_button >= SAPP_MOUSEBUTTON_LEFT &&
			    event->mouse_button <= SAPP_MOUSEBUTTON_MIDDLE)
			{
				video.mouse_buttons[event->mouse_button] = event->type == SAPP_EVENTTYPE_MOUSE_DOWN;
			}
			return;
		}
		if (event->key_code == SAPP_KEYCODE_INVALID)
		{
			return;
		}
		if (event->type == SAPP_EVENTTYPE_KEY_DOWN)
		{
			video.keys[event->key_code] = true;
		}
		else if (event->type == SAPP_EVENTTYPE_KEY_UP)
		{
			video.keys[event->key_code] = false;
		}
	}

	void cleanup_cb()
	{
		sg_shutdown();
		if (audio.started)
		{
			saudio_shutdown();
			audio.started = false;
		}
	}

} // namespace

static Atom frame_loop(VmState& s, Atom title, Atom width, Atom height, Atom emulation, Atom frame)
{
	JET_DIE_UNLESS(&s, video.vm == nullptr, "frame-loop: already running");

	const std::string& emulation_name = *slow_unbox<Symbol>(s, emulation);
	if (emulation_name == "none")
	{
		video.screen_emulation = ScreenEmulation::None;
	}
	else if (emulation_name == "crt")
	{
		video.screen_emulation = ScreenEmulation::Crt;
	}
	else
	{
		JET_DIE(&s, "frame-loop: unknown screen emulation '{}', expected 'none' or 'crt'", emulation_name);
	}

	video.vm = &s;
	video.frame_proc = frame;
	video.width = static_cast<int>(slow_unbox<Number>(s, width));
	video.height = static_cast<int>(slow_unbox<Number>(s, height));
	JET_DIE_UNLESS(&s, video.width > 0 && video.height > 0, "frame-loop: window is {}x{}", video.width,
	               video.height);

	s.stack_top = s.stack_base + s.frames.back().top;

	sapp_desc app{};
	app.init_cb = init_cb;
	app.frame_cb = frame_cb;
	app.cleanup_cb = cleanup_cb;
	app.event_cb = event_cb;
	app.width = video.width * WINDOW_SCALE;
	app.height = static_cast<int>(video.height * WINDOW_SCALE * PIXEL_ASPECT);
	app.fullscreen = true;
	app.window_title = slow_unbox<String>(s, title)->c_str();
	app.logger.func = slog_func;
	sapp_run(&app);

	vm_exit(s, 0);
}

static Atom display_framebuffer(VmState& s, Atom pixels)
{
	JET_DIE_UNLESS(&s, video.ready, "display-framebuffer: no window; call frame-loop first");

	ByteVector& bytes = *slow_unbox<ByteVector>(s, pixels);
	size_t expected = static_cast<size_t>(video.width) * static_cast<size_t>(video.height);
	JET_DIE_UNLESS(&s, bytes.size() == expected, "display-framebuffer: {} bytes, expected {}",
	               bytes.size(),
	               expected);

	sg_image_data upload{};
	upload.mip_levels[0] = sg_range{bytes.data(), bytes.size()};
	sg_update_image(video.indices, &upload);

	if (video.palette_dirty)
	{
		sg_image_data palette_upload{};
		palette_upload.mip_levels[0] = sg_range{video.rgba, sizeof(video.rgba)};
		sg_update_image(video.palette, &palette_upload);
		video.palette_dirty = false;
	}

	sg_pass pass{};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = {0.0f, 0.0f, 0.0f, 1.0f};
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);

	sg_bindings bindings{};
	bindings.vertex_buffers[0] = video.quad;
	bindings.views[0] = video.index_view;
	bindings.views[1] = video.palette_view;
	bindings.samplers[0] = video.sampler;
	sg_apply_pipeline(video.pipeline);
	sg_apply_bindings(&bindings);
	sg_draw(0, 4, 1);

	sg_end_pass();
	sg_commit();

	return Atom{};
}

static Atom set_palette(VmState& s, Atom colors)
{
	ByteVector& bytes = *slow_unbox<ByteVector>(s, colors);
	JET_DIE_UNLESS(&s, bytes.size() == PALETTE_COLORS * 3, "set-palette: {} bytes, expected {}",
	               bytes.size(),
	               PALETTE_COLORS * 3);

	for (size_t index{0}; index < PALETTE_COLORS; index++)
	{
		for (size_t channel{0}; channel < 3; channel++)
		{
			uint8_t six_bit = bytes[index * 3 + channel];
			video.rgba[index * 4 + channel] = static_cast<uint8_t>((six_bit << 2) | (six_bit >> 4));
		}
		video.rgba[index * 4 + 3] = 255;
	}

	// sokol allows one update per image and frame, and a fade sets the palette many times per frame,
	// so the upload waits for the next framebuffer display.
	video.palette_dirty = true;

	return Atom{};
}

static Atom play_sound(VmState& s, Atom channel, Atom pcm, Atom rate)
{
	size_t slot = voice_index(s, channel);
	ByteVector& bytes = *slow_unbox<ByteVector>(s, pcm);
	double hertz = slow_unbox<Number>(s, rate);
	JET_DIE_UNLESS(&s, hertz > 0.0, "play-sound: rate is {}", hertz);
	JET_DIE_UNLESS(&s, !bytes.empty(), "play-sound: no samples");

	// Without a device nothing drains the voice, so it must never report itself as playing.
	if (!start_audio())
	{
		return Atom{};
	}

	Voice fresh{};
	fresh.samples.assign(bytes.begin(), bytes.end());
	fresh.step = hertz / saudio_sample_rate();
	fresh.playing = true;

	const std::lock_guard<std::mutex> held{audio.lock};
	audio.voices[slot] = std::move(fresh);

	return Atom{};
}

static uint8_t adlib_byte(VmState& s, Atom value, const char* name)
{
	double number = slow_unbox<Number>(s, value);
	JET_DIE_UNLESS(&s,
	               std::isfinite(number) && number >= 0.0 && number <= 255.0 && std::floor(number) == number,
	               "{}: {}, expected an integer from 0 to 255", name, number);

	return static_cast<uint8_t>(number);
}

static bool adlib_reset(VmState&)
{
	if (!start_audio())
	{
		return false;
	}

	const std::lock_guard<std::mutex> held{audio.lock};
	audio.adlib.reset();
	audio.adlib_step = 0.0;
	audio.adlib_phase = 1.0;
	audio.adlib_sample = 0.0f;

	return true;
}

static Atom adlib_write(VmState& s, Atom register_value, Atom data_value)
{
	uint8_t register_byte = adlib_byte(s, register_value, "adlib-write register");
	uint8_t data_byte = adlib_byte(s, data_value, "adlib-write value");
	JET_DIE_UNLESS(&s, start_audio(), "adlib-write: audio device is unavailable");

	const std::lock_guard<std::mutex> held{audio.lock};
	audio.adlib.write_address(register_byte);
	audio.adlib.write_data(data_byte);

	return Atom{};
}

static Atom set_sound_attenuation(VmState& s, Atom left, Atom right)
{
	int left_step = static_cast<int>(slow_unbox<Number>(s, left));
	int right_step = static_cast<int>(slow_unbox<Number>(s, right));
	JET_DIE_UNLESS(&s, left_step >= 0 && left_step <= MAX_ATTENUATION && right_step >= 0 &&
	               right_step <= MAX_ATTENUATION,
	               "set-sound-attenuation: {} and {}, expected 0 to {}", left_step, right_step,
	               MAX_ATTENUATION);

	const std::lock_guard<std::mutex> held{audio.lock};
	audio.left = static_cast<float>(MAX_ATTENUATION - left_step) / MAX_ATTENUATION;
	audio.right = static_cast<float>(MAX_ATTENUATION - right_step) / MAX_ATTENUATION;

	return Atom{};
}

static Atom stop_sound(VmState& s, Atom channel)
{
	size_t slot = voice_index(s, channel);

	const std::lock_guard<std::mutex> held{audio.lock};
	audio.voices[slot].playing = false;

	return Atom{};
}

static bool sound_playing(VmState& s, Atom channel)
{
	size_t slot = voice_index(s, channel);

	const std::lock_guard<std::mutex> held{audio.lock};
	return audio.voices[slot].playing;
}

static bool key_down(VmState& s, Atom key)
{
	return video.keys[key_code(s, key)];
}

static Number get_mouse_motion_x(VmState&)
{
	double delta{video.mouse_dx};
	video.mouse_dx = 0.0;
	return Number::from_ieee(delta);
}

static Number get_mouse_motion_y(VmState&)
{
	double delta{video.mouse_dy};
	video.mouse_dy = 0.0;
	return Number::from_ieee(delta);
}

static bool mouse_button_down(VmState& s, Atom button)
{
	const std::string& name{*slow_unbox<Symbol>(s, button)};
	if (name == "left")
	{
		return video.mouse_buttons[SAPP_MOUSEBUTTON_LEFT];
	}
	if (name == "right")
	{
		return video.mouse_buttons[SAPP_MOUSEBUTTON_RIGHT];
	}
	if (name == "middle")
	{
		return video.mouse_buttons[SAPP_MOUSEBUTTON_MIDDLE];
	}

	JET_DIE(&s, "mouse-button-down?: unknown button '{}'", name);
}

static Atom set_window_title(VmState& s, Atom title)
{
	sapp_set_window_title(slow_unbox<String>(s, title)->c_str());
	return Atom{};
}

static Atom request_quit(VmState&)
{
	sapp_request_quit();
	return Atom{};
}

void init_dos(VmState& s)
{
	Env& e = s.env;
	e.bind("dos:frame-loop", make_prim<frame_loop>(s));
	e.bind("dos:display-framebuffer", make_prim<display_framebuffer>(s));
	e.bind("dos:set-palette", make_prim<set_palette>(s));
	e.bind("dos:key-down?", make_prim<key_down>(s));
	e.bind("dos:get-mouse-motion-x", make_prim<get_mouse_motion_x>(s));
	e.bind("dos:get-mouse-motion-y", make_prim<get_mouse_motion_y>(s));
	e.bind("dos:mouse-button-down?", make_prim<mouse_button_down>(s));
	e.bind("dos:play-sound", make_prim<play_sound>(s));
	e.bind("dos:stop-sound", make_prim<stop_sound>(s));
	e.bind("dos:adlib-reset", make_prim<adlib_reset>(s));
	e.bind("dos:adlib-write", make_prim<adlib_write>(s));
	e.bind("dos:set-sound-attenuation", make_prim<set_sound_attenuation>(s));
	e.bind("dos:sound-playing?", make_prim<sound_playing>(s));
	e.bind("dos:set-window-title", make_prim<set_window_title>(s));
	e.bind("dos:request-quit", make_prim<request_quit>(s));
}
