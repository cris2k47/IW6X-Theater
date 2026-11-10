#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "command.hpp"
#include "console.hpp"
#include "scheduler.hpp"
#include "demo_playback.hpp"
#include "theater_gamepad.hpp"

#include "game/game.hpp"
#include "utils/hook.hpp"

// Developer tools (opt-in: create the file <game>\theater\dev\enable):
//  - command pipe: lines written to theater\dev\commands.txt are executed as console commands
//    ('!' prefixed lines are internal actions, e.g. "!dump")
//  - state file: theater\dev\state.txt is refreshed twice per second
//  - theater_dumpimage: writes the decrypted game image (for reverse engineering) to theater\dev

namespace devtools
{
	namespace
	{
		std::filesystem::path dev_dir()
		{
			return console::get_theater_dir() / "dev";
		}

		// copy of everything printed to the game console (Lua errors included) -> theater\dev\console.log
		utils::hook::detour conbuf_append_text_hook;
		std::mutex console_log_mutex;

		void conbuf_append_text_stub(const char* text)
		{
			if (text)
			{
				std::lock_guard _(console_log_mutex);
				static std::ofstream file(dev_dir() / "console.log", std::ios::out | std::ios::trunc);
				file << text;
				file.flush();
			}

			conbuf_append_text_hook.invoke<void>(text);
		}

		bool dev_enabled()
		{
			std::error_code ec;
			return std::filesystem::exists(dev_dir() / "enable", ec);
		}

		void dump_image()
		{
			const auto base = reinterpret_cast<std::uint8_t*>(0x140000000);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto size = nt->OptionalHeader.SizeOfImage;

			std::vector<std::uint8_t> image(size, 0);
			std::size_t copied = 0;
			for (std::size_t off = 0; off < size;)
			{
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(base + off, &mbi, sizeof(mbi)))
				{
					break;
				}

				const auto region_end = std::min<std::size_t>(reinterpret_cast<std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - base, size);
				const auto readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && mbi.Protect != 0;
				if (readable)
				{
					std::memcpy(image.data() + off, base + off, region_end - off);
					copied += region_end - off;
				}

				off = region_end;
			}

			// raw layout == virtual layout so the dump can be loaded as a regular PE
			auto* out_dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
			auto* out_nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + out_dos->e_lfanew);
			auto* section = IMAGE_FIRST_SECTION(out_nt);
			for (auto i = 0; i < out_nt->FileHeader.NumberOfSections; ++i, ++section)
			{
				section->PointerToRawData = section->VirtualAddress;
				section->SizeOfRawData = std::min<DWORD>(section->Misc.VirtualSize, size - section->VirtualAddress);
			}
			out_nt->OptionalHeader.FileAlignment = out_nt->OptionalHeader.SectionAlignment;

			std::error_code ec;
			std::filesystem::create_directories(dev_dir(), ec);
			const auto path = dev_dir() / "iw6mp64_dump.exe";
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
			file.close();

			console::info("dumped game image (%zu of %u bytes readable) to %s\n", copied, size, path.string().data());
		}

		void write_state()
		{
			std::ostringstream s;
			s << "time_ms=" << GetTickCount64() << "\n";
			s << "connstate=" << static_cast<int>(*game::mp::connstate) << "\n";
			s << "cgame_initialized=" << game::CL_IsCgameInitialized() << "\n";
			s << "demo_playing=" << demo_playback::playing() << "\n";
			s << "cl.serverTime=" << game::mp::cl->serverTime << "\n";
			s << "cl.snap.serverTime=" << game::mp::cl->snap.serverTime << "\n";
			s << "cl.snap.valid=" << game::mp::cl->snap.valid << "\n";
			s << "keyCatchers=" << *game::keyCatchers << "\n";

			if (const auto* mapname = game::Dvar_FindVar("mapname"); mapname && mapname->current.string)
			{
				s << "mapname=" << mapname->current.string << "\n";
			}

			if (const auto* timescale = game::Dvar_FindVar("demotimescale"); timescale)
			{
				s << "demotimescale=" << timescale->current.value << "\n";
			}

			const auto tmp = dev_dir() / "state.tmp";
			{
				std::ofstream file(tmp, std::ios::trunc);
				file << s.str();
			}

			std::error_code ec;
			std::filesystem::rename(tmp, dev_dir() / "state.txt", ec);
		}

		int key_from_name(const std::string& name)
		{
			static const std::unordered_map<std::string, int> keys =
			{
				{"enter", game::K_ENTER}, {"escape", game::K_ESCAPE}, {"esc", game::K_ESCAPE}, {"space", game::K_SPACE},
				{"tab", game::K_TAB}, {"up", game::K_UPARROW}, {"down", game::K_DOWNARROW}, {"left", game::K_LEFTARROW},
				{"right", game::K_RIGHTARROW}, {"backspace", game::K_BACKSPACE}, {"shift", game::K_SHIFT},
				{"ctrl", game::K_CTRL}, {"alt", game::K_ALT}, {"mouse1", game::K_MOUSE1}, {"f1", game::K_F1},
				{"f2", game::K_F2}, {"f3", game::K_F3}, {"f4", game::K_F4}, {"home", game::K_HOME},
				{"pgup", game::K_PGUP}, {"pgdn", game::K_PGDN}, {"del", game::K_DEL}, {"mouse2", game::K_MOUSE2},
				{"wheelup", game::K_MWHEELUP}, {"wheeldown", game::K_MWHEELDOWN},
			};

			if (const auto it = keys.find(name); it != keys.end())
			{
				return it->second;
			}

			if (name.size() == 1)
			{
				return static_cast<unsigned char>(name[0]);
			}

			return std::atoi(name.data());
		}

		// "!key enter,down,enter" or "!key w:hold500" -> injects key presses through CL_KeyEvent
		void inject_keys(const std::string& spec)
		{
			std::stringstream ss(spec);
			std::string item;
			auto delay = 0ms;
			while (std::getline(ss, item, ','))
			{
				auto hold = 60ms;
				if (const auto colon = item.find(":hold"); colon != std::string::npos)
				{
					hold = std::chrono::milliseconds(std::atoi(item.data() + colon + 5));
					item = item.substr(0, colon);
				}

				const auto key = key_from_name(item);
				scheduler::once([key]()
				{
					game::CL_KeyEvent(0, key, 1, static_cast<unsigned int>(game::Sys_Milliseconds()));
				}, scheduler::pipeline::main, delay);
				scheduler::once([key]()
				{
					game::CL_KeyEvent(0, key, 0, static_cast<unsigned int>(game::Sys_Milliseconds()));
				}, scheduler::pipeline::main, delay + hold);

				delay += hold + 150ms;
			}
		}

		void execute_line(const std::string& line)
		{
			console::log("pipe: " + line);
			if (line == "!dump")
			{
				dump_image();
			}
			else if (line == "!state")
			{
				write_state();
			}
			else if (line.starts_with("!key "))
			{
				inject_keys(line.substr(5));
			}
			else if (line.starts_with("!pad "))
			{
				// "!pad A" / "!pad LB,DPAD_UP" / "!pad lx=0 ly=1 hold=1000" -> virtual controller for the theater
				static const std::unordered_map<std::string, std::uint16_t> buttons_by_name{
					{"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}, {"LB", 0x0100}, {"RB", 0x0200},
					{"L3", 0x0040}, {"R3", 0x0080}, {"VIEW", 0x0020}, {"START", 0x0010}, {"DPAD_UP", 0x0001},
					{"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
				};

				std::uint16_t buttons{};
				float lx{}, ly{}, rx{}, ry{}, lt{}, rt{};
				auto hold = 120;

				std::istringstream s(line.substr(5));
				std::string token;
				while (s >> token)
				{
					const auto eq = token.find('=');
					if (eq == std::string::npos)
					{
						std::stringstream names(token);
						std::string name;
						while (std::getline(names, name, ','))
						{
							if (const auto it = buttons_by_name.find(name); it != buttons_by_name.end()) buttons |= it->second;
						}
						continue;
					}

					const auto name = token.substr(0, eq);
					const auto value = static_cast<float>(std::atof(token.data() + eq + 1));
					if (name == "lx") lx = value;
					else if (name == "ly") ly = value;
					else if (name == "rx") rx = value;
					else if (name == "ry") ry = value;
					else if (name == "lt") lt = value;
					else if (name == "rt") rt = value;
					else if (name == "hold") hold = static_cast<int>(value);
				}

				theater_gamepad::inject(buttons, lx, ly, rx, ry, lt, rt, std::chrono::milliseconds(hold));
			}
			else if (line.starts_with("!mouse "))
			{
				// "!mouse dx dy" -> one relative mouse movement through CL_MouseEvent (same path as raw input)
				int dx{}, dy{};
				if (std::sscanf(line.data() + 7, "%d %d", &dx, &dy) == 2)
				{
					game::CL_MouseEvent(0, 0, dx, dy);
				}
			}
			else
			{
				command::execute(line);
			}
		}

		void process_pipe()
		{
			const auto path = dev_dir() / "commands.txt";
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
			{
				return;
			}

			std::vector<std::string> lines;
			{
				std::ifstream file(path);
				std::string line;
				while (std::getline(file, line))
				{
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (!line.empty()) lines.push_back(line);
				}
			}
			std::filesystem::remove(path, ec);

			auto delay = 0ms;
			for (const auto& line : lines)
			{
				if (line.starts_with("!wait "))
				{
					delay += std::chrono::milliseconds(std::atoi(line.data() + 6));
					continue;
				}

				scheduler::once([line]()
				{
					execute_line(line);
				}, scheduler::pipeline::main, delay);
			}
		}
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			command::add("theater_dumpimage", []()
			{
				dump_image();
			});

			// same as LUI's Engine.NotifyServer(name, value): "cmd lui <netconst index> <value> <serverId>"
			// (used to pick team / class without the menus while testing)
			command::add("theater_dev_luinotify", [](const command::params& params)
			{
				if (params.size() < 3)
				{
					console::info("usage: theater_dev_luinotify <name> <value>\n");
					return;
				}

				using get_index_t = int(*)(int, const char*, int*);
				const auto get_index = reinterpret_cast<get_index_t>(0x1402079D0);
				const auto server_id = *reinterpret_cast<int*>(0x141D9BA3C);

				int index{};
				if (!get_index(0x15, params[1], &index))
				{
					console::error("'%s' is not a LUI notify name\n", params[1]);
					return;
				}

				command::execute(std::format("cmd lui {} {} {}", index, std::atoi(params[2]), server_id));
			});

			if (!dev_enabled())
			{
				return;
			}

			console::info("developer tools enabled\n");

			// theater_dev_listassets <type id> [substring]  -> theater\dev\assets.txt (material = 5, lua file = 54)
			command::add("theater_dev_listassets", [](const command::params& params)
			{
				if (params.size() < 2) return;

				struct context
				{
					std::string filter;
					std::vector<std::string> names;
				} ctx{params.size() > 2 ? params[2] : ""};

				game::DB_EnumXAssets_Internal(static_cast<game::XAssetType>(std::atoi(params[1])), [](game::XAssetHeader header, void* data)
				{
					auto& c = *static_cast<context*>(data);
					const auto* name = header.data ? *static_cast<const char* const*>(header.data) : nullptr;
					if (!name) return;
					std::string lower = name;
					std::ranges::transform(lower, lower.begin(), ::tolower);
					if (c.filter.empty() || lower.find(c.filter) != std::string::npos)
					{
						c.names.emplace_back(name);
					}
				}, &ctx, true);

				std::ranges::sort(ctx.names);
				std::ofstream file(dev_dir() / "assets.txt", std::ios::app);
				file << "== type " << params[1] << " filter '" << ctx.filter << "': " << ctx.names.size() << "\n";
				for (const auto& n : ctx.names) file << n << "\n";
				console::info("%zu assets listed\n", ctx.names.size());
			});

			// theater_dev_glyphtest -> draws the controller glyphs of fonts/normalfont with their codes
			command::add("theater_dev_glyphtest", []()
			{
				static auto enabled = false;
				enabled = !enabled;
				if (!enabled) return;

				scheduler::schedule([]()
				{
					if (!enabled) return scheduler::cond_end;

					auto* font = game::R_RegisterFont("fonts/normalfont");
					auto* white = game::Material_RegisterHandle("white");
					static constexpr float bg[4]{0.0f, 0.0f, 0.0f, 0.85f};
					static constexpr float fg[4]{1.0f, 1.0f, 1.0f, 1.0f};
					game::R_AddCmdDrawStretchPic(80.0f, 80.0f, 1400.0f, 220.0f, 0.0f, 0.0f, 0.0f, 0.0f, const_cast<float*>(bg), white);

					static constexpr std::array<int, 18> codes{0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
					auto x = 100.0f;
					for (const auto code : codes)
					{
						const char glyph_text[2]{static_cast<char>(code), 0};
						game::R_AddCmdDrawText(glyph_text, 1, font, x, 190.0f, 1.2f, 1.2f, 0.0f, fg, 0);
						const auto label = std::format("{:x}", code);
						game::R_AddCmdDrawText(label.data(), 8, font, x, 260.0f, 0.5f, 0.5f, 0.0f, fg, 0);
						x += 75.0f;
					}

					return scheduler::cond_continue;
				}, scheduler::pipeline::renderer);
			});

			// theater_dev_fonttest -> draws a sample line in every loaded font
			command::add("theater_dev_fonttest", []()
			{
				static auto enabled = false;
				enabled = !enabled;
				if (!enabled) return;

				scheduler::schedule([]()
				{
					if (!enabled) return scheduler::cond_end;

					static constexpr std::array fonts{
						"fonts/normalFont", "fonts/smallFont", "fonts/bigFont", "fonts/boldFont", "fonts/extraBigFont",
						"fonts/objectiveFont", "fonts/hudBigFont", "fonts/hudNormalFont", "fonts/hudSmallFont",
						"fonts/hudDigitalFont20", "fonts/consoleFont",
					};

					auto* white = game::Material_RegisterHandle("white");
					static constexpr float bg[4]{0.0f, 0.0f, 0.0f, 0.85f};
					static constexpr float fg[4]{1.0f, 1.0f, 1.0f, 1.0f};
					static constexpr float yellow[4]{1.0f, 0.85f, 0.2f, 1.0f};
					game::R_AddCmdDrawStretchPic(20.0f, 20.0f, 1500.0f, 60.0f * fonts.size() + 20.0f, 0.0f, 0.0f, 0.0f, 0.0f, const_cast<float*>(bg), white);

					auto y = 70.0f;
					for (const auto* name : fonts)
					{
						auto* font = game::R_RegisterFont(name);
						if (font)
						{
							const auto scale = 34.0f / static_cast<float>(font->pixelHeight);
							game::R_AddCmdDrawText(name, 64, font, 40.0f, y, scale * 0.6f, scale * 0.6f, 0.0f, yellow, 0);
							game::R_AddCmdDrawText("1ST PERSON  RECORD  SCREENSHOT  00:21  Toggle Controls  MODE: Film Editor", 128, font, 400.0f, y, scale, scale, 0.0f, fg, 0);
						}
						y += 60.0f;
					}

					return scheduler::cond_continue;
				}, scheduler::pipeline::renderer);
			});

			// theater_dev_fontinfo <font name> -> lists the glyph codes below 0x80 that the font contains
			command::add("theater_dev_fontinfo", [](const command::params& params)
			{
				if (params.size() < 2) return;
				auto* font = game::R_RegisterFont(params[1]);
				std::ofstream file(dev_dir() / "assets.txt", std::ios::app);
				if (!font)
				{
					file << "font " << params[1] << " not found\n";
					return;
				}

				file << "font " << params[1] << " pixelHeight=" << font->pixelHeight << " glyphCount=" << font->glyphCount << " low glyphs:";
				for (auto i = 0; i < font->glyphCount; ++i)
				{
					const auto letter = font->glyphs[i].letter;
					if (letter < 0x20 || letter > 0xFF) file << std::format(" {:#x}", letter);
				}
				file << "\n";
			});

			// theater_dev_dumplua <substring> -> theater\dev\lua\<name> (compiled LUI scripts in memory)
			command::add("theater_dev_dumplua", [](const command::params& params)
			{
				struct context
				{
					std::string filter;
					int count{};
				} ctx{params.size() > 1 ? params[1] : ""};

				game::DB_EnumXAssets_Internal(game::ASSET_TYPE_LUA_FILE, [](game::XAssetHeader header, void* data)
				{
					auto& c = *static_cast<context*>(data);
					const auto* lua = static_cast<const game::LuaFile*>(header.data);
					if (!lua || !lua->name || !lua->buffer) return;
					const std::string name = lua->name;
					if (!c.filter.empty() && name.find(c.filter) == std::string::npos) return;

					auto path = dev_dir() / "lua" / name;
					std::error_code ec;
					std::filesystem::create_directories(path.parent_path(), ec);
					std::ofstream file(path, std::ios::binary | std::ios::trunc);
					file.write(lua->buffer, lua->len);
					++c.count;
				}, &ctx, true);

				console::info("%d lua files dumped\n", ctx.count);
			});

			// never execute commands left over from a previous session
			std::error_code stale_ec;
			std::filesystem::remove(dev_dir() / "commands.txt", stale_ec);

			conbuf_append_text_hook.create(game::Conbuf_AppendText.get(), conbuf_append_text_stub);

			scheduler::loop(process_pipe, scheduler::pipeline::main, 200ms);
			scheduler::loop(write_state, scheduler::pipeline::main, 500ms);
		}
	};
}

REGISTER_COMPONENT(devtools::component)
