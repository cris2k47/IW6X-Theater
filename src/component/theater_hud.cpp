#include <std_include.hpp>

#include "theater.hpp"
#include "theater_markers.hpp"
#include "theater_world.hpp"
#include "game/game.hpp"

// Theater HUD: a copy of the Black Ops II theater ("Film Editor") control panel, measured on Plutonium T6.
//
//    F4 Toggle Controls          F Insert Camera Marker          F2 Toggle Hud
//   +-------------------------------------------------------------------------+
//   |          (M1) cris2k47 (M2)                     MODE: Film Editor        |
//   |                   01:19                                                 |
//   |   [====|====|====|===v=|====|====|====|====|====|====|====]   02:08     |
//   |   [ 1ST PERSON ] [ |<  [>  ||  >>  >| ] [  RECORD  ] [ SCREENSHOT ]     |
//   |        F3          <-   v  SPACE  ^  ->       R           Hold R        |
//   +-------------------------------------------------------------------------+
//
// Controller: (LS) Toggle Controls, (B) Toggle HUD, LB <name> RB, hints Y | LT dpad-down A dpad-up dpad-right | X |
// Hold X (the D-pad up / down speed sits around A like Down / Up around SPACE). The keyboard or controller prompts
// follow the device used last. Also draws the BO2 popups (NOTICE, CAMERA MARKER, Keep Clip), short messages and, in
// the world, the attach highlight and the 2D part of the Edit Camera Mode markers (theater_markers.cpp).

namespace theater_hud
{
	using namespace theater::math;

	namespace
	{
		std::mutex message_mutex;
		std::string message;
		std::chrono::steady_clock::time_point message_time{};

		// ------------------------------------------------------------------ colors (BO2 theater palette)

		constexpr float panel_bg[4]{0.07f, 0.07f, 0.075f, 0.72f};
		constexpr float panel_border[4]{1.0f, 1.0f, 1.0f, 0.10f};
		constexpr float button_bg[4]{0.16f, 0.16f, 0.17f, 0.92f};
		constexpr float button_border[4]{0.0f, 0.0f, 0.0f, 0.65f};
		constexpr float record_bg[4]{0.90f, 0.08f, 0.08f, 0.95f};
		constexpr float text_white[4]{0.96f, 0.96f, 0.96f, 1.0f};
		constexpr float text_disabled[4]{0.42f, 0.42f, 0.42f, 1.0f};
		constexpr float key_yellow[4]{1.0f, 0.86f, 0.30f, 1.0f};
		constexpr float speed_yellow[4]{1.0f, 0.88f, 0.15f, 1.0f};
		constexpr float select_orange[4]{0.93f, 0.45f, 0.10f, 1.0f};
		constexpr float bar_bg[4]{0.0f, 0.0f, 0.0f, 0.88f};
		constexpr float bar_border[4]{0.55f, 0.55f, 0.55f, 0.85f};
		constexpr float tick_white[4]{0.85f, 0.85f, 0.85f, 0.85f};
		constexpr float tick_green[4]{0.30f, 0.95f, 0.30f, 1.0f};
		constexpr float tick_red[4]{0.85f, 0.10f, 0.10f, 1.0f};
		constexpr float tick_yellow[4]{1.0f, 0.86f, 0.20f, 1.0f};
		constexpr float icon_white[4]{0.95f, 0.95f, 0.95f, 1.0f};
		constexpr float icon_disabled[4]{0.38f, 0.38f, 0.38f, 1.0f};
		constexpr float msg_color[4]{1.0f, 0.86f, 0.30f, 1.0f};
		constexpr float attach_green[4]{0.30f, 1.0f, 0.30f, 0.95f};
		constexpr float dim_screen[4]{0.0f, 0.0f, 0.0f, 0.55f};
		constexpr float popup_bg[4]{0.13f, 0.13f, 0.14f, 0.97f};
		constexpr float popup_top[4]{0.28f, 0.28f, 0.30f, 1.0f};
		constexpr float popup_border[4]{0.45f, 0.45f, 0.47f, 0.9f};
		constexpr float mouse_body[4]{0.82f, 0.82f, 0.82f, 1.0f};
		constexpr float mouse_button[4]{0.93f, 0.50f, 0.12f, 1.0f};

		// ------------------------------------------------------------------ panel geometry (BO2, 1600x900 capture; times scale)

		constexpr float PANEL_W = 612.0f;
		constexpr float PANEL_H = 128.0f;
		constexpr float PANEL_BOTTOM = 14.0f; // gap under the panel
		constexpr float PROMPTS_H = 30.0f;    // prompt row above the panel (Toggle Controls, centre prompt, Toggle Hud)

		// ------------------------------------------------------------------ primitives

		game::Material* white()
		{
			static auto* material = game::Material_RegisterHandle("white");
			return material;
		}

		// fonts/smallFont is the game's condensed face (closest to BO2's theater font) and also
		// contains the controller button glyphs
		game::Font_t* font()
		{
			static auto* f = game::R_RegisterFont("fonts/smallFont");
			return f;
		}

		void rect(const float x, const float y, const float w, const float h, const float* color)
		{
			game::R_AddCmdDrawStretchPic(x, y, w, h, 0.0f, 0.0f, 0.0f, 0.0f, const_cast<float*>(color), white());
		}

		void frame(const float x, const float y, const float w, const float h, const float t, const float* color)
		{
			rect(x, y, w, t, color);
			rect(x, y + h - t, w, t, color);
			rect(x, y + t, t, h - 2.0f * t, color);
			rect(x + w - t, y + t, t, h - 2.0f * t, color);
		}

		float text_width(const std::string& text, const float scale)
		{
			return static_cast<float>(game::R_TextWidth(text.data(), std::numeric_limits<int>::max(), font())) * scale;
		}

		float text_height(const float scale)
		{
			return static_cast<float>(font()->pixelHeight) * scale;
		}

		void text(const std::string& str, const float x, const float baseline, const float scale, const float* color)
		{
			game::R_AddCmdDrawText(str.data(), std::numeric_limits<int>::max(), font(), x, baseline, scale, scale, 0.0f, color, 6);
		}

		// draws text centered horizontally on cx and vertically inside [top, top + h]
		void text_centered(const std::string& str, const float cx, const float top, const float h, const float scale, const float* color)
		{
			const auto baseline = top + (h + text_height(scale) * 0.70f) * 0.5f;
			text(str, cx - text_width(str, scale) * 0.5f, baseline, scale, color);
		}

		void text_left(const std::string& str, const float x, const float top, const float h, const float scale, const float* color)
		{
			const auto baseline = top + (h + text_height(scale) * 0.70f) * 0.5f;
			text(str, x, baseline, scale, color);
		}

		// filled triangle made of thin vertical slices; dir = +1 points right, -1 points left
		void triangle_h(const float x, const float cy, const float w, const float h, const int dir, const float* color)
		{
			const auto slices = std::max(1, static_cast<int>(w));
			const auto slice_w = w / static_cast<float>(slices);
			for (auto i = 0; i < slices; ++i)
			{
				const auto t = (static_cast<float>(i) + 0.5f) / static_cast<float>(slices);
				const auto column_h = h * (dir > 0 ? 1.0f - t : t);
				rect(x + static_cast<float>(i) * slice_w, cy - column_h * 0.5f, slice_w + 0.5f, column_h, color);
			}
		}

		// filled triangle made of thin horizontal slices; dir = +1 points down, -1 points up
		void triangle_v(const float cx, const float y, const float w, const float h, const int dir, const float* color)
		{
			const auto slices = std::max(1, static_cast<int>(h));
			const auto slice_h = h / static_cast<float>(slices);
			for (auto i = 0; i < slices; ++i)
			{
				const auto t = (static_cast<float>(i) + 0.5f) / static_cast<float>(slices);
				const auto row_w = w * (dir > 0 ? 1.0f - t : t);
				rect(cx - row_w * 0.5f, y + static_cast<float>(i) * slice_h, row_w, slice_h + 0.5f, color);
			}
		}

		// yellow BO2-style arrow key hint
		void arrow(const float cx, const float cy, const float size, const char direction, const float* color)
		{
			const auto head = size * 0.55f;
			const auto stem_len = size * 0.5f;
			const auto stem_w = std::max(2.0f, size * 0.16f);

			switch (direction)
			{
			case 'l':
				triangle_h(cx - size * 0.5f, cy, head, size * 0.7f, -1, color);
				rect(cx - size * 0.5f + head - 1.0f, cy - stem_w * 0.5f, stem_len, stem_w, color);
				break;
			case 'r':
				triangle_h(cx + size * 0.5f - head, cy, head, size * 0.7f, +1, color);
				rect(cx + size * 0.5f - head - stem_len + 1.0f, cy - stem_w * 0.5f, stem_len, stem_w, color);
				break;
			case 'u':
				triangle_v(cx, cy - size * 0.5f, size * 0.7f, head, -1, color);
				rect(cx - stem_w * 0.5f, cy - size * 0.5f + head - 1.0f, stem_w, stem_len, color);
				break;
			default:
				triangle_v(cx, cy + size * 0.5f - head, size * 0.7f, head, +1, color);
				rect(cx - stem_w * 0.5f, cy + size * 0.5f - head - stem_len + 1.0f, stem_w, stem_len, color);
				break;
			}
		}

		// small mouse with the left or right button highlighted (the BO2 player selector)
		void mouse_icon(const float cx, const float cy, const float scale, const bool right)
		{
			const auto w = 11.0f * scale;
			const auto h = 16.0f * scale;
			const auto x = cx - w * 0.5f;
			const auto y = cy - h * 0.5f;
			rect(x + 1.0f * scale, y, w - 2.0f * scale, h, mouse_body);
			rect(x, y + 2.0f * scale, w, h - 5.0f * scale, mouse_body);
			const auto button_h = h * 0.42f;
			rect(right ? cx + 0.75f * scale : x + 1.0f * scale, y + 1.0f * scale, w * 0.5f - 1.75f * scale, button_h, mouse_button);
			rect(cx - 0.5f * scale, y + 1.0f * scale, 1.0f * scale, button_h, button_border);
		}

		// ------------------------------------------------------------------ controller glyphs (fonts/smallFont)
		// The glyph code is the engine's key number of the button (K_BUTTON_A 0x01 .. K_DPAD_RIGHT 0x17). Checked in the
		// font's atlas image itself (gamefonts_pc, BC3 1024 x 1404, decoded from eng_code_post_gfx_mp.ff and cut with the
		// glyph rectangles of fonts/smallFont, pixelHeight 35): 0x01-0x06 A B X Y LB RB, 0x0E Start, 0x0F Back,
		// 0x10 / 0x11 LS / RS, 0x12 / 0x13 LT / RT, 0x14-0x17 the D-pad with its up / down / left / right arrow lit.
		// 0x18 / 0x19 are a second LS / RS image (the one used here).

		namespace pad_glyph
		{
			constexpr auto A = "\x01";
			constexpr auto B = "\x02";
			constexpr auto X = "\x03";
			constexpr auto Y = "\x04";
			constexpr auto LB = "\x05";
			constexpr auto RB = "\x06";
			constexpr auto LT = "\x12";
			constexpr auto DPAD_UP = "\x14";
			constexpr auto DPAD_DOWN = "\x15";
			constexpr auto DPAD_RIGHT = "\x17";
			constexpr auto LS = "\x18";
			constexpr auto RS = "\x19";
		}

		bool gamepad_hints()
		{
			return theater::last_input_device() == theater::input_device::gamepad;
		}

		// ------------------------------------------------------------------ transport icons (BO2 shapes)

		enum class icon
		{
			jump_back,   // |<
			slow,        // [>
			play,        // >
			pause,       // ||
			fast,        // >>
			jump_forward // >|
		};

		void draw_icon(const icon type, const float cx, const float cy, const float size, const float* color)
		{
			const auto h = size;
			const auto tri_w = size * 0.78f;
			const auto bar_w = std::max(2.0f, size * 0.24f);

			switch (type)
			{
			case icon::jump_back:
				rect(cx - tri_w * 0.5f - bar_w, cy - h * 0.5f, bar_w, h, color);
				triangle_h(cx - tri_w * 0.5f + 1.0f, cy, tri_w, h, -1, color);
				break;
			case icon::slow:
				rect(cx - tri_w * 0.5f - bar_w - 1.0f, cy - h * 0.5f, bar_w * 1.4f, h, color);
				triangle_h(cx - tri_w * 0.5f + bar_w * 0.6f, cy, tri_w, h, +1, color);
				break;
			case icon::play:
				triangle_h(cx - tri_w * 0.45f, cy, tri_w, h, +1, color);
				break;
			case icon::pause:
				rect(cx - bar_w * 1.5f, cy - h * 0.5f, bar_w * 1.1f, h, color);
				rect(cx + bar_w * 0.4f, cy - h * 0.5f, bar_w * 1.1f, h, color);
				break;
			case icon::fast:
				triangle_h(cx - tri_w, cy, tri_w, h, +1, color);
				triangle_h(cx, cy, tri_w, h, +1, color);
				break;
			case icon::jump_forward:
				triangle_h(cx - tri_w * 0.5f - 1.0f, cy, tri_w, h, +1, color);
				rect(cx + tri_w * 0.5f, cy - h * 0.5f, bar_w, h, color);
				break;
			}
		}

		std::string format_time(const double ms)
		{
			const auto total = static_cast<int>(std::max(0.0, ms) / 1000.0);
			return std::format("{:02}:{:02}", total / 60, total % 60);
		}

		std::string format_speed(const float speed)
		{
			return std::format("{:.1f}x", speed);
		}

		// ------------------------------------------------------------------ world (Edit Camera Mode markers, attach highlight)

		bool project(const vec3& point, const vec3& origin, const vec3 axis[3], const float tan_x, const float tan_y,
		             const float width, const float height, float& sx, float& sy)
		{
			const auto d = point - origin;
			const auto depth = dot(d, axis[0]);
			if (depth < 1.0)
			{
				return false;
			}

			const auto x = dot(d, axis[1]) / (depth * tan_x);
			const auto y = dot(d, axis[2]) / (depth * tan_y);
			sx = static_cast<float>(width * 0.5 * (1.0 - x));
			sy = static_cast<float>(height * 0.5 * (1.0 - y));
			return sx > -50.0f && sx < width + 50.0f && sy > -50.0f && sy < height + 50.0f;
		}

		void draw_world(const float width, const float height, const float scale)
		{
			vec3 origin{};
			vec3 axis[3]{};
			float tan_x{}, tan_y{};
			if (!theater::get_render_camera(origin, axis, tan_x, tan_y))
			{
				return;
			}

			// Free Roam: the player "Attach To Object" would follow is highlighted in green
			if (const auto client = theater::attach_candidate(); client >= 0)
			{
				if (const auto feet = theater::world::origin(client))
				{
					float sx{}, sy{};
					if (project(*feet + vec3{0.0, 0.0, 78.0}, origin, axis, tan_x, tan_y, width, height, sx, sy))
					{
						triangle_v(sx, sy - 12.0f * scale, 16.0f * scale, 12.0f * scale, +1, attach_green);
					}
				}
			}

			// BO2 shows the camera markers and their track only in Edit Camera Mode: the track, the "#N" / "MM:SS" labels
			// and (on maps without the camera model) the cameras are 2D, the camera models and cones are 3D. The labels
			// keep out of the panel and its prompt row, which draw_panel puts on top of them
			if (theater::get_view_mode() == theater::view_mode::free_camera && theater::get_free_mode() == theater::free_mode::edit)
			{
				theater::markers::screen_rect keep_out{};
				if (theater::controls_visible())
				{
					keep_out.w = PANEL_W * scale;
					keep_out.h = (PROMPTS_H + PANEL_H + PANEL_BOTTOM) * scale;
					keep_out.x = (width - keep_out.w) * 0.5f;
					keep_out.y = height - keep_out.h;
				}

				theater::markers::draw_hud(width, height, scale, keep_out);
			}
		}

		// ------------------------------------------------------------------ popups

		struct popup_box
		{
			float x, y, w, h, scale;
		};

		popup_box popup_frame(const float width, const float height, const float scale, const float w, const float h)
		{
			rect(0.0f, 0.0f, width, height, dim_screen);
			const popup_box box{(width - w * scale) * 0.5f, (height - h * scale) * 0.5f - 20.0f * scale, w * scale, h * scale, scale};
			rect(box.x, box.y, box.w, box.h, popup_bg);
			rect(box.x, box.y, box.w, 3.0f * scale, popup_top);
			frame(box.x, box.y, box.w, box.h, std::max(1.0f, scale), popup_border);
			return box;
		}

		// footer prompt under the box: "<key> <label>"
		void popup_footer(const popup_box& box, const std::string& keyboard_key, const char* pad_key, const std::string& label)
		{
			const auto pad = gamepad_hints();
			const auto key = pad ? std::string(pad_key) : keyboard_key;
			const auto s = box.scale * 1.15f;
			const auto top = box.y + box.h + 6.0f * box.scale;
			const auto h = 30.0f * box.scale;
			text_left(key, box.x + 8.0f * box.scale, top, h, pad ? box.scale * 1.2f : s, pad ? text_white : key_yellow);
			text_left(label, box.x + 14.0f * box.scale + text_width(key, pad ? box.scale * 1.2f : s), top, h, s * 0.9f, text_white);
		}

		std::vector<std::string> wrap(const std::string& str, const float max_width, const float scale)
		{
			std::vector<std::string> lines;
			std::istringstream words(str);
			std::string word, line;
			while (words >> word)
			{
				const auto candidate = line.empty() ? word : line + " " + word;
				if (!line.empty() && text_width(candidate, scale) > max_width)
				{
					lines.push_back(line);
					line = word;
				}
				else
				{
					line = candidate;
				}
			}

			if (!line.empty())
			{
				lines.push_back(line);
			}

			return lines;
		}

		void draw_notice(const float width, const float height, const float scale)
		{
			const auto box = popup_frame(width, height, scale, 600.0f, 300.0f);
			text_left("NOTICE", box.x + 20.0f * scale, box.y + 14.0f * scale, 50.0f * scale, scale * 2.0f, text_white);

			const auto body_scale = scale * 1.25f;
			auto y = box.y + 78.0f * scale;
			for (const auto& line : wrap("You have switched to the Dolly Camera, but you have not placed any markers. "
			                             "Make sure you place markers in the Edit Camera Mode.", box.w - 40.0f * scale, body_scale))
			{
				text_left(line, box.x + 20.0f * scale, y, 30.0f * scale, body_scale, text_white);
				y += 34.0f * scale;
			}

			popup_footer(box, "ENTER", pad_glyph::A, "Accept");
		}

		void draw_marker_popup(const float width, const float height, const float scale)
		{
			const auto& popup = theater::get_popup();
			const auto& keys = theater::get_keyframes();
			if (popup.marker < 0 || popup.marker >= static_cast<int>(keys.size()))
			{
				return;
			}

			const auto& k = keys[popup.marker];
			const auto rows = theater::marker_rows(popup.marker);
			const auto box = popup_frame(width, height, scale, 600.0f, 300.0f);
			const auto pad = gamepad_hints();

			// header: previous / next marker
			const auto header_top = box.y + 10.0f * scale;
			const auto header_h = 40.0f * scale;
			text_centered(std::format("CAMERA MARKER # {}", popup.marker + 1), box.x + box.w * 0.5f, header_top, header_h, scale * 1.55f, text_white);
			if (pad)
			{
				text_left(pad_glyph::LB, box.x + 18.0f * scale, header_top, header_h, scale, text_white);
				text_left(pad_glyph::RB, box.x + box.w - 18.0f * scale - text_width(pad_glyph::RB, scale), header_top, header_h, scale, text_white);
			}
			else
			{
				triangle_h(box.x + 22.0f * scale, header_top + header_h * 0.5f, 10.0f * scale, 16.0f * scale, -1, text_white);
				triangle_h(box.x + box.w - 32.0f * scale, header_top + header_h * 0.5f, 10.0f * scale, 16.0f * scale, +1, text_white);
			}

			const auto row_h = 36.0f * scale;
			auto y = box.y + 56.0f * scale;
			const auto label_scale = scale * 1.35f;
			const auto left = box.x + 20.0f * scale;
			const auto value_x = box.x + box.w * 0.6f;

			for (auto i = 0; i < static_cast<int>(rows.size()); ++i)
			{
				const auto selected = i == popup.selection;
				const auto* color = selected ? select_orange : text_white;
				switch (rows[i])
				{
				case theater::marker_row::speed_mode:
				{
					static constexpr std::array names{"Manual", "Linear", "Fixed"};
					text_left("Speed Mode", left, y, row_h, label_scale, color);
					text_left(names[static_cast<int>(k.mode)], value_x, y, row_h, label_scale, color);
					y += row_h;
					break;
				}
				case theater::marker_row::speed:
					text_left(std::format("Speed ( {:.1f}x )", k.speed), left, y, row_h, label_scale, color);
					y += row_h;
					break;
				case theater::marker_row::reposition:
					y += row_h * 0.5f;
					text_left("Reposition Camera", left, y, row_h, label_scale, color);
					y += row_h;
					break;
				case theater::marker_row::remove:
					text_left("Remove Marker", left, y, row_h, label_scale, color);
					y += row_h;
					break;
				case theater::marker_row::remove_all:
					y += row_h * 0.5f;
					text_left(popup.confirm && selected ? "Remove All Markers? Press again" : "Remove All Markers", left, y, row_h, label_scale, color);
					y += row_h;
					break;
				}
			}

			popup_footer(box, "ESC", pad_glyph::B, "Done");
		}

		void draw_keep_clip(const float width, const float height, const float scale)
		{
			const auto& popup = theater::get_popup();
			const auto box = popup_frame(width, height, scale, 600.0f, 300.0f);
			text_left("Keep Clip", box.x + 20.0f * scale, box.y + 14.0f * scale, 50.0f * scale, scale * 2.0f, text_white);

			static constexpr std::array rows{"Autoname and Save Clip", "Name and Save Clip", "Preview Clip", "Discard Clip"};
			auto y = box.y + 128.0f * scale;
			for (auto i = 0; i < static_cast<int>(rows.size()); ++i)
			{
				text_left(rows[i], box.x + 20.0f * scale, y, 38.0f * scale, scale * 1.35f, i == popup.selection ? select_orange : text_white);
				y += 38.0f * scale;
			}
		}

		void draw_name_clip(const float width, const float height, const float scale)
		{
			const auto& popup = theater::get_popup();
			const auto box = popup_frame(width, height, scale, 600.0f, 220.0f);
			text_left("Name and Save Clip", box.x + 20.0f * scale, box.y + 14.0f * scale, 50.0f * scale, scale * 2.0f, text_white);

			const auto field_y = box.y + 100.0f * scale;
			rect(box.x + 20.0f * scale, field_y, box.w - 40.0f * scale, 44.0f * scale, bar_bg);
			frame(box.x + 20.0f * scale, field_y, box.w - 40.0f * scale, 44.0f * scale, std::max(1.0f, scale), bar_border);
			const auto blink = (std::chrono::steady_clock::now().time_since_epoch() / 500ms) % 2 == 0;
			text_left(popup.text + (blink ? "_" : " "), box.x + 30.0f * scale, field_y, 44.0f * scale, scale * 1.1f, text_white);

			const auto top = box.y + box.h + 6.0f * scale;
			const auto h = 30.0f * scale;
			auto x = box.x + 8.0f * scale;
			for (const auto& [k, label] : {std::pair{"ENTER", "Save"}, std::pair{"ESC", "Back"}})
			{
				text_left(k, x, top, h, scale * 0.95f, key_yellow);
				x += text_width(k, scale * 0.95f) + 6.0f * scale;
				text_left(label, x, top, h, scale * 0.85f, text_white);
				x += text_width(label, scale * 0.85f) + 24.0f * scale;
			}
		}

		void draw_popup(const float width, const float height, const float scale)
		{
			switch (theater::get_popup().kind)
			{
			case theater::popup_kind::dolly_notice: draw_notice(width, height, scale); break;
			case theater::popup_kind::marker: draw_marker_popup(width, height, scale); break;
			case theater::popup_kind::keep_clip: draw_keep_clip(width, height, scale); break;
			case theater::popup_kind::name_clip: draw_name_clip(width, height, scale); break;
			default: break;
			}
		}

		// ------------------------------------------------------------------ panel

		// "<key> <label>" prompt; key yellow on keyboard, a controller glyph on the pad
		float prompt_width(const std::string& key, const std::string& label, const float key_scale, const float label_scale)
		{
			return text_width(key, key_scale) + 6.0f * label_scale + text_width(label, label_scale);
		}

		void prompt(const std::string& key, const std::string& label, const float x, const float top, const float h,
		            const float key_scale, const float label_scale, const bool pad)
		{
			text_left(key, x, top, h, key_scale, pad ? text_white : key_yellow);
			text_left(label, x + text_width(key, key_scale) + 6.0f * label_scale, top, h, label_scale, text_white);
		}

		std::string centre_prompt(const bool pad, std::string& key)
		{
			using namespace theater;
			if (get_popup().kind != popup_kind::none || get_view_mode() != view_mode::free_camera)
			{
				return {};
			}

			key = pad ? pad_glyph::RS : "F";
			if (get_free_mode() == free_mode::edit)
			{
				if (repositioning_marker()) return "Place Camera Marker";
				return near_marker() >= 0 ? "Edit Camera Marker" : "Insert Camera Marker";
			}

			if (get_free_mode() == free_mode::free_roam)
			{
				if (attach_target() >= 0) return "Detach From Object";
				if (attach_candidate() >= 0) return "Attach To Object";
			}

			return {};
		}

		void draw_panel(const float width, const float height, const float scale)
		{
			using namespace theater;
			const auto pad = gamepad_hints();
			const auto& info = get_demo_info();

			// BO2 geometry (1600x900 capture): panel 612 x 128 centered 10 px above the bottom
			const auto panel_w = PANEL_W * scale;
			const auto panel_h = PANEL_H * scale;
			const auto panel_x = (width - panel_w) * 0.5f;
			const auto panel_y = height - panel_h - PANEL_BOTTOM * scale;
			const auto label_scale = scale * 0.70f;
			const auto key_scale = pad ? scale * 0.80f : scale * 0.72f;

			// ---------------------------------------------------------------- prompts above the panel
			{
				const auto top = panel_y - PROMPTS_H * scale;
				const auto h = 28.0f * scale;
				prompt(pad ? pad_glyph::LS : "F4", "Toggle Controls", panel_x, top, h, key_scale, label_scale, pad);

				const std::string hud_label = pad ? "Toggle HUD" : "Toggle Hud";
				const std::string hud_key = pad ? pad_glyph::B : "F2";
				const auto hud_w = prompt_width(hud_key, hud_label, key_scale, label_scale);
				prompt(hud_key, hud_label, panel_x + panel_w - hud_w, top, h, key_scale, label_scale, pad);

				std::string key;
				if (const auto label = centre_prompt(pad, key); !label.empty())
				{
					const auto w = prompt_width(key, label, key_scale, label_scale);
					prompt(key, label, panel_x + (panel_w - w) * 0.5f, top, h, key_scale, label_scale, pad);
				}
			}

			rect(panel_x, panel_y, panel_w, panel_h, panel_bg);
			frame(panel_x, panel_y, panel_w, panel_h, std::max(1.0f, scale), panel_border);

			// ---------------------------------------------------------------- row 1: selector, mode
			const auto row1_top = panel_y + 2.0f * scale;
			const auto row1_h = 24.0f * scale;
			{
				const auto cx = panel_x + panel_w * 0.30f;
				const auto name = selector_text();
				const auto name_w = text_width(name, label_scale);
				text_centered(name, cx, row1_top, row1_h, label_scale, text_white);

				const auto left_x = cx - name_w * 0.5f - 14.0f * scale;
				const auto right_x = cx + name_w * 0.5f + 14.0f * scale;
				const auto mid_y = row1_top + row1_h * 0.5f;
				if (pad)
				{
					text_centered(pad_glyph::LB, left_x, row1_top, row1_h, scale * 0.9f, text_white);
					text_centered(pad_glyph::RB, right_x, row1_top, row1_h, scale * 0.9f, text_white);
				}
				else
				{
					mouse_icon(left_x, mid_y, scale, false);
					mouse_icon(right_x, mid_y, scale, true);
				}

				const char* mode = previewing_clip() ? "MODE: Previewing Clip" : "MODE: Film Editor";
				text_centered(mode, panel_x + panel_w * 0.775f, row1_top, row1_h, label_scale, text_white);
			}

			// ---------------------------------------------------------------- rows 2 + 3: time, timeline, total
			const auto bar_x = panel_x + 12.0f * scale;
			const auto bar_w = panel_w - 76.0f * scale;
			const auto bar_h = 13.0f * scale;
			const auto bar_y = row1_top + row1_h + 22.0f * scale;
			{
				const auto now = get_smooth_time();
				const auto length = info.has_length() ? static_cast<double>(info.last_time - info.first_time) : 0.0;
				const auto to_x = [&](const double server_time)
				{
					const auto p = length > 0.0 ? std::clamp((server_time - info.first_time) / length, 0.0, 1.0) : 0.0;
					return bar_x + 2.0f * scale + static_cast<float>((bar_w - 4.0f * scale) * p);
				};

				rect(bar_x, bar_y, bar_w, bar_h, bar_bg);
				frame(bar_x, bar_y, bar_w, bar_h, std::max(1.0f, scale), bar_border);

				const auto inner_y = bar_y + 2.0f * scale;
				const auto inner_h = bar_h - 4.0f * scale;

				if (length > 0.0)
				{
					// viewed player: red before the first spawn, deaths red, kills green
					const auto marks = get_life_marks();
					if (marks.first_spawn > info.first_time)
					{
						rect(to_x(info.first_time), inner_y, to_x(marks.first_spawn) - to_x(info.first_time), inner_h, tick_red);
					}

					// 10 s marks
					for (auto t = 10000.0; t < length; t += 10000.0)
					{
						rect(to_x(info.first_time + t), inner_y, std::max(1.0f, scale), inner_h, tick_white);
					}

					for (const auto death : marks.deaths)
					{
						rect(to_x(death) - 1.0f * scale, inner_y, 2.0f * scale, inner_h, tick_red);
					}

					for (const auto kill : marks.kills)
					{
						rect(to_x(kill) - 1.0f * scale, inner_y, 2.0f * scale, inner_h, tick_green);
					}

					// camera markers (free camera only)
					if (get_view_mode() == view_mode::free_camera)
					{
						for (const auto& k : get_keyframes())
						{
							rect(to_x(k.time) - 1.0f * scale, inner_y, 2.0f * scale, inner_h, tick_yellow);
						}
					}
				}

				// play head: line through the bar, small white triangle above, time above it
				const auto head_x = to_x(now);
				rect(head_x - 1.0f * scale, bar_y, 2.0f * scale, bar_h, icon_white);
				triangle_v(head_x, bar_y - 6.0f * scale, 10.0f * scale, 6.0f * scale, +1, icon_white);
				text_centered(format_time(now - info.first_time), std::clamp(head_x, bar_x + 18.0f * scale, bar_x + bar_w - 18.0f * scale),
				              bar_y - 25.0f * scale, 20.0f * scale, label_scale, text_white);

				const auto total = info.has_length() ? format_time(length) : std::string("--:--");
				text_left(total, bar_x + bar_w + 6.0f * scale, bar_y - 6.0f * scale, bar_h + 12.0f * scale, label_scale, text_white);
			}

			// ---------------------------------------------------------------- row 4: buttons
			const auto buttons_y = bar_y + bar_h + 4.0f * scale;
			const auto buttons_h = 27.0f * scale;
			const auto camera_w = 140.0f * scale;
			const auto transport_w = 210.0f * scale;
			const auto record_w = 96.0f * scale;
			const auto shot_w = 118.0f * scale;
			const auto camera_x = panel_x + 16.0f * scale;
			const auto transport_x = camera_x + camera_w + 4.0f * scale;
			const auto record_x = transport_x + transport_w + 4.0f * scale;
			const auto shot_x = record_x + record_w + 4.0f * scale;

			const auto view = get_view_mode();
			const auto fmode = get_free_mode();
			const auto recording = recording_clip();
			const auto record_enabled = !(view == view_mode::free_camera && fmode == free_mode::edit);
			const auto shot_enabled = !recording && !(view == view_mode::free_camera && fmode != free_mode::free_roam);

			const auto button = [&](const float x, const float w, const float* bg)
			{
				rect(x, buttons_y, w, buttons_h, bg);
				frame(x, buttons_y, w, buttons_h, std::max(1.0f, scale), button_border);
			};

			button(camera_x, camera_w, button_bg);
			const char* camera_label = view == view_mode::first_person ? "1ST PERSON" : view == view_mode::third_person ? "3RD PERSON" : "FREE CAMERA";
			text_centered(camera_label, camera_x + camera_w * 0.5f, buttons_y, buttons_h, label_scale, text_white);

			// transport: |<  [>  ||/>  >>  >|  (the speed text replaces [> below 1x, >> from 1x)
			button(transport_x, transport_w, button_bg);
			const auto slot = transport_w / 5.0f;
			const auto icon_size = 13.0f * scale;
			const auto icon_y = buttons_y + buttons_h * 0.5f;
			const auto slot_x = [&](const int i) { return transport_x + slot * (static_cast<float>(i) + 0.5f); };

			const auto ff = fast_forwarding();
			const auto speed = ff ? fast_forward_speed() : get_speed();
			const auto show_speed = ff || speed_label_visible();
			const auto* speed_color = ff ? speed_yellow : text_white;

			draw_icon(icon::jump_back, slot_x(0), icon_y, icon_size, can_jump_back() ? icon_white : icon_disabled);
			if (show_speed && speed < 1.0f)
				text_centered(format_speed(speed), slot_x(1), buttons_y, buttons_h, label_scale, speed_color);
			else
				draw_icon(icon::slow, slot_x(1), icon_y, icon_size, icon_white);
			draw_icon(is_paused() ? icon::play : icon::pause, slot_x(2), icon_y, icon_size, icon_white);
			if (show_speed && speed >= 1.0f)
				text_centered(format_speed(speed), slot_x(3), buttons_y, buttons_h, label_scale, speed_color);
			else
				draw_icon(icon::fast, slot_x(3), icon_y, icon_size, icon_white);
			draw_icon(icon::jump_forward, slot_x(4), icon_y, icon_size, can_jump_forward() ? icon_white : icon_disabled);

			button(record_x, record_w, recording ? record_bg : button_bg);
			text_centered("RECORD", record_x + record_w * 0.5f, buttons_y, buttons_h, label_scale, record_enabled || recording ? text_white : text_disabled);

			button(shot_x, shot_w, button_bg);
			text_centered("SCREENSHOT", shot_x + shot_w * 0.5f, buttons_y, buttons_h, label_scale, shot_enabled ? text_white : text_disabled);

			// ---------------------------------------------------------------- row 5: keys under the buttons
			const auto hints_y = buttons_y + buttons_h;
			const auto hints_h = panel_y + panel_h - hints_y;
			const auto hint_scale = pad ? scale * 0.80f : scale * 0.72f;
			const auto* hint_color = pad ? text_white : key_yellow;

			text_centered(pad ? pad_glyph::Y : "F3", camera_x + camera_w * 0.5f, hints_y, hints_h, hint_scale, hint_color);

			if (pad)
			{
				// like the keyboard's Down / Up around SPACE: D-pad down under the slow slot, D-pad up under the fast one
				// (RT still fast forwards while held)
				const std::array glyphs{pad_glyph::LT, pad_glyph::DPAD_DOWN, pad_glyph::A, pad_glyph::DPAD_UP, pad_glyph::DPAD_RIGHT};
				for (auto i = 0; i < 5; ++i)
				{
					text_centered(glyphs[i], slot_x(i), hints_y, hints_h, hint_scale, text_white);
				}
			}
			else
			{
				const auto arrow_size = 14.0f * scale;
				const auto mid = hints_y + hints_h * 0.5f;
				arrow(slot_x(0), mid, arrow_size, 'l', key_yellow);
				arrow(slot_x(1), mid, arrow_size, 'd', key_yellow);
				text_centered("SPACE", slot_x(2), hints_y, hints_h, hint_scale, key_yellow);
				arrow(slot_x(3), mid, arrow_size, 'u', key_yellow);
				arrow(slot_x(4), mid, arrow_size, 'r', key_yellow);
			}

			text_centered(pad ? pad_glyph::X : "R", record_x + record_w * 0.5f, hints_y, hints_h, hint_scale, hint_color);

			// "Hold R" / "Hold X": Hold in white, the key in yellow (or the glyph)
			{
				const std::string hold = "Hold ";
				const std::string k = pad ? pad_glyph::X : "R";
				const auto w = text_width(hold, label_scale) + text_width(k, hint_scale);
				const auto x = shot_x + (shot_w - w) * 0.5f;
				text_left(hold, x, hints_y, hints_h, label_scale, text_white);
				text_left(k, x + text_width(hold, label_scale), hints_y, hints_h, hint_scale, hint_color);
			}
		}
	}

	void show_message(const std::string& msg)
	{
		std::lock_guard _(message_mutex);
		message = msg;
		message_time = std::chrono::steady_clock::now();
	}

	void draw()
	{
		const auto* placement = game::ScrPlace_GetViewPlacement();
		const auto width = placement->realViewportSize[0];
		const auto height = placement->realViewportSize[1];
		const auto scale = std::max(0.6f, height / 900.0f);

		draw_world(width, height, scale);

		if (theater::controls_visible())
		{
			draw_panel(width, height, scale);
		}

		// transient message
		{
			std::lock_guard _(message_mutex);
			const auto age = std::chrono::steady_clock::now() - message_time;
			if (!message.empty() && age < 2500ms)
			{
				const auto s = scale * 0.95f;
				text(message, (width - text_width(message, s)) * 0.5f, height * 0.16f, s, msg_color);
			}
		}

		draw_popup(width, height, scale);
	}
}
