-- iw6-theater: "THEATER" entry in the multiplayer main menu with a list of recorded demos, the THEATER SETTINGS menu
-- and the "GAME RECORDING" option in the private match lobby (like Black Ops II).
-- Installed to <game>\iw6\ui_scripts\theater\__init__.lua (outside the launcher-managed data\ folder).
-- Demo playback, cameras and the timeline are handled by the native add-on (XINPUT1_3.dll).

local DEMO_FOLDERS = {
	{ path = "demos/client/user/", tag = "" },
	{ path = "demos/client/auto/", tag = "[auto] " },
	{ path = "demos/server/", tag = "[server] " },
}

local SETTINGS_MENU = "theater_settings_menu"

-- dvars registered by the native add-on; the saved ones are written to players2\config_mp.cfg by the game
local DVARS = {
	game_recording = "demoautorecord",
	all_players = "theater_pov_record",
	video_on_save = "theater_video_on_save",
	video_fps = "theater_video_fps",
	video_quality = "theater_video_quality",
	video_bitrate = "theater_video_bitrate",
	camera_speed = "theater_freecam_speed",
	mouse_scale = "theater_mouse_scale",
	film = "theater_film", -- read only: 1 while a film plays
}

local TEXT = {
	settings = "Settings",
	settings_title = "THEATER SETTINGS",
	on = "ON",
	off = "OFF",
	custom = "CUSTOM",
	mbps = " MBPS",
	fps = " FPS",
	no_demos = "NO DEMOS YET - PLAY A MATCH WITH GAME RECORDING ON",
	game_recording = "GAME RECORDING",
	game_recording_desc = "Turn on/off the recording of your game.",
	all_players = "RECORD ALL PLAYERS",
	all_players_desc = "As the host, also save every player's view to watch anyone in 1st person.",
	video_on_save = "SAVE VIDEO WITH CLIPS",
	video_on_save_desc = "Make an MP4 video in Videos\\IW6X Theater every time you save a clip.",
	video_fps = "VIDEO FRAME RATE",
	video_fps_desc = "Frames per second of the videos. More fps takes longer to make.",
	video_quality = "VIDEO QUALITY",
	video_quality_desc = "Bitrate for your {size} window at {fps} fps. More quality, bigger files.",
	video_quality_desc_any = "Bitrate for your window size and frame rate. More quality, bigger files.",
	video_quality_desc_custom = "Bitrate set in the console (theater_video_bitrate). Left / Right: presets.",
	camera_speed = "FREE CAMERA SPEED",
	camera_speed_desc = "Movement speed of the free camera.",
	mouse_scale = "FREE CAMERA SENSITIVITY",
	mouse_scale_desc = "Mouse sensitivity of the free camera, on top of your game sensitivity.",
}

local FPS_VALUES = { 24, 25, 30, 48, 50, 60, 90, 120, 144, 240 }
local CAMERA_SPEED_VALUES = { 50, 75, 100, 150, 250, 400, 600, 1000, 1500, 2500, 5000 }
local MOUSE_SCALE = { min = 0.25, max = 3, step = 0.25 }

-- the video quality presets (theater_video_quality 0..4) and bitrate limits of the add-on's video encoder
-- (theater_video.cpp): the bitrate is width x height x fps x these bits per pixel, so it follows the size and the fps
local QUALITY_PRESETS = {
	{ name = "LOW", bits_per_pixel = 0.1 },
	{ name = "MEDIUM", bits_per_pixel = 0.2 },
	{ name = "HIGH", bits_per_pixel = 0.3 },
	{ name = "ULTRA", bits_per_pixel = 0.5 },
	{ name = "MAX", bits_per_pixel = 0.8 },
}
local MIN_BITRATE_MBPS = 8
local MAX_BITRATE_MBPS = 150

-- sent to the whole menu after every change, so values that depend on others (the bitrate on the fps) update too
local SETTINGS_REFRESH_EVENT = "theater_settings_refresh"

-- ------------------------------------------------------------------ dvar helpers

-- Engine.GetDvarBool / GetDvarInt / GetDvarFloat / GetDvarString return nil for a dvar that does not exist (the add-on
-- is missing or older than this script), but each one only works on its own dvar type: on another type the value is
-- read as a string pointer and the game crashes (pcall cannot catch that). So every dvar is read with its own getter.
local function dvar_exists( name, get )
	return get( name ) ~= nil
end

local function settings_available()
	return dvar_exists( DVARS.video_fps, Engine.GetDvarInt )
end

local function on_off( enabled )
	if enabled then
		return TEXT.on
	end
	return TEXT.off
end

-- next value of 'values' in 'direction' from 'current' (which may be any value typed in the console)
local function step_in_list( values, current, direction )
	if direction > 0 then
		for i = 1, #values do
			if values[i] > current + 0.001 then
				return values[i]
			end
		end
		return values[#values]
	end
	for i = #values, 1, -1 do
		if values[i] < current - 0.001 then
			return values[i]
		end
	end
	return values[1]
end

local function whole( value )
	return tostring( math.floor( value + 0.5 ) )
end

local function clamp_mbps( mbps )
	return math.min( MAX_BITRATE_MBPS, math.max( MIN_BITRATE_MBPS, mbps ) )
end

-- the video has the size of the game window; r_mode is an enum dvar ("1600x900"), so GetDvarString is safe on it
local function video_size()
	local width, height = string.match( Engine.GetDvarString( "r_mode" ) or "", "(%d+)x(%d+)" )
	if width then
		return tonumber( width ), tonumber( height )
	end
	return nil, nil
end

local function quality_index()
	return math.min( #QUALITY_PRESETS, math.max( 1, (Engine.GetDvarInt( DVARS.video_quality ) or 2) + 1 ) )
end

-- a bitrate typed in the console (theater_video_bitrate > 0) overrides the presets
local function custom_bitrate()
	local mbps = Engine.GetDvarInt( DVARS.video_bitrate )
	if mbps and mbps > 0 then
		return mbps
	end
	return nil
end

local function quality_text()
	local custom = custom_bitrate()
	if custom then
		return TEXT.custom .. " (" .. whole( clamp_mbps( custom ) ) .. TEXT.mbps .. ")"
	end
	local preset = QUALITY_PRESETS[quality_index()]
	local width, height = video_size()
	local fps = Engine.GetDvarInt( DVARS.video_fps )
	if not width or not fps then
		return preset.name
	end
	return preset.name .. " (" .. whole( clamp_mbps( width * height * fps * preset.bits_per_pixel / 1000000 ) ) .. TEXT.mbps .. ")"
end

local function quality_description()
	if custom_bitrate() then
		return TEXT.video_quality_desc_custom
	end
	local width, height = video_size()
	local fps = Engine.GetDvarInt( DVARS.video_fps )
	if not width or not fps then
		return TEXT.video_quality_desc_any
	end
	local text = string.gsub( TEXT.video_quality_desc, "{size}", whole( width ) .. "x" .. whole( height ) )
	text = string.gsub( text, "{fps}", whole( fps ) )
	return text
end

-- ------------------------------------------------------------------ option buttons

local function glass_properties( label, description )
	return {
		style = GenericButtonSettings.Styles.GlassButton,
		substyle = GenericButtonSettings.Styles.GlassButton.SubStyles.SubMenu,
		button_text = label,
		desc_text = description,
		text_padding_with_content = 10,
		text_align_with_content = LUI.Alignment.Left
	}
end

local function option_button( dvar, properties )
	return {
		type = "UIGenericButton",
		id = "theater_option_" .. dvar,
		properties = properties,
		handlers = {
			element_refresh = MBh.EmitEvent( "content_refresh" ),
			[SETTINGS_REFRESH_EVENT] = MBh.EmitEvent( "content_refresh" )
		}
	}
end

-- every option redraws its value (the selector only redraws the one that changed)
local function settings_changed( f15_arg0 )
	f15_arg0:dispatchEventToRoot( {
		name = SETTINGS_REFRESH_EVENT,
		immediate = true
	} )
end

-- 'change(direction)' sets the dvar; Left / Right and the arrows call it, then every option is redrawn and, when the
-- description is a function of the value, the description line too (the game only updates it on focus)
local function selector_option( dvar, properties, display, change )
	local function changed( element, direction )
		change( direction )
		settings_changed( element )
		if type( properties.desc_text ) == "function" then
			element:dispatchEventToRoot( {
				name = "set_button_info_text",
				text = properties.desc_text()
			} )
		end
	end

	properties.variant = GenericButtonSettings.Variants.Select
	properties.button_display_func = display
	properties.button_left_func = function( f16_arg0 )
		changed( f16_arg0, -1 )
	end
	properties.button_right_func = function( f17_arg0 )
		changed( f17_arg0, 1 )
	end
	return option_button( dvar, properties )
end

-- ON / OFF with the arrows; A, Enter or a click also switch it. 'properties' carries the text and the style.
local function toggle_option( dvar, properties )
	local function toggle()
		Engine.SetDvarBool( dvar, not Engine.GetDvarBool( dvar ) )
	end

	local button = selector_option( dvar, properties, function()
		return on_off( Engine.GetDvarBool( dvar ) )
	end, toggle )
	properties.button_action_func = function( f18_arg0 )
		toggle()
		settings_changed( f18_arg0 )
	end
	return button
end

local function list_option( dvar, label, description, values, get, set, text )
	return selector_option( dvar, glass_properties( label, description ), function()
		return text( get( dvar ) )
	end, function( direction )
		set( dvar, step_in_list( values, get( dvar ), direction ) )
	end )
end

-- the quality presets; from a bitrate typed in the console, Left / Right go back to the presets
local function quality_option()
	return selector_option( DVARS.video_quality, glass_properties( TEXT.video_quality, quality_description ), quality_text,
		function( direction )
			if custom_bitrate() then
				Engine.SetDvarInt( DVARS.video_bitrate, 0 )
			else
				Engine.SetDvarInt( DVARS.video_quality, math.min( #QUALITY_PRESETS, math.max( 1, quality_index() + direction ) ) - 1 )
			end
		end )
end

local function slider_option( dvar, label, description, range )
	local function change( direction )
		local value = Engine.GetDvarFloat( dvar ) + direction * range.step
		value = math.floor( value / range.step + 0.5 ) * range.step -- back on the grid after a value typed in the console
		Engine.SetDvarFloat( dvar, math.min( range.max, math.max( range.min, value ) ) )
	end

	local properties = glass_properties( label, description )
	properties.variant = GenericButtonSettings.Variants.Slider
	properties.button_display_func = function()
		local value = Engine.GetDvarFloat( dvar )
		return math.min( 1, math.max( 0, (value - range.min) / (range.max - range.min) ) )
	end
	properties.button_left_func = function()
		change( -1 )
	end
	properties.button_right_func = function()
		change( 1 )
	end
	return option_button( dvar, properties )
end

-- ------------------------------------------------------------------ THEATER SETTINGS menu

-- the option rows (without the description line) of the options the add-on has
local function settings_options()
	local items = {}

	if dvar_exists( DVARS.game_recording, Engine.GetDvarBool ) then
		items[#items + 1] = toggle_option( DVARS.game_recording, glass_properties( TEXT.game_recording, TEXT.game_recording_desc ) )
	end
	if dvar_exists( DVARS.all_players, Engine.GetDvarBool ) then
		items[#items + 1] = toggle_option( DVARS.all_players, glass_properties( TEXT.all_players, TEXT.all_players_desc ) )
	end
	if dvar_exists( DVARS.video_on_save, Engine.GetDvarBool ) then
		items[#items + 1] = toggle_option( DVARS.video_on_save, glass_properties( TEXT.video_on_save, TEXT.video_on_save_desc ) )
	end
	if dvar_exists( DVARS.video_fps, Engine.GetDvarInt ) then
		items[#items + 1] = list_option( DVARS.video_fps, TEXT.video_fps, TEXT.video_fps_desc, FPS_VALUES,
			Engine.GetDvarInt, Engine.SetDvarInt, function ( value )
				return whole( value ) .. TEXT.fps
			end )
	end
	if dvar_exists( DVARS.video_quality, Engine.GetDvarInt ) and dvar_exists( DVARS.video_bitrate, Engine.GetDvarInt ) then
		items[#items + 1] = quality_option()
	end
	if dvar_exists( DVARS.camera_speed, Engine.GetDvarFloat ) then
		items[#items + 1] = list_option( DVARS.camera_speed, TEXT.camera_speed, TEXT.camera_speed_desc, CAMERA_SPEED_VALUES,
			Engine.GetDvarFloat, Engine.SetDvarFloat, whole )
	end
	if dvar_exists( DVARS.mouse_scale, Engine.GetDvarFloat ) then
		items[#items + 1] = slider_option( DVARS.mouse_scale, TEXT.mouse_scale, TEXT.mouse_scale_desc, MOUSE_SCALE )
	end

	return items
end

-- the description of the focused option, under the list
local function description_line()
	if Engine.InFrontend() then
		return {
			type = "button_desc_text",
			id = "theater_settings_description"
		}
	end

	-- in a film the game's button_desc_text fails an assert: a text filled by the event the buttons send on focus
	return {
		type = "UIText",
		id = "theater_settings_description",
		properties = {
			text = ""
		},
		states = {
			default = CoD.ColorizeState( Colors.generic_button_text_default_color, {
				leftAnchor = true,
				rightAnchor = true,
				topAnchor = true,
				bottomAnchor = false,
				left = 14,
				right = -14,
				top = 0,
				height = CoD.TextSettings.NormalFont.Height + 6,
				font = CoD.TextSettings.NormalFont.Font,
				alignment = LUI.Alignment.Left
			} )
		},
		handlers = {
			set_button_info_text = function ( f20_arg0, f20_arg1 )
				f20_arg0:setText( f20_arg1.text or "" )
			end
		}
	}
end

local function SettingsFeeder( f8_arg0 )
	local items = settings_options()
	items[#items + 1] = description_line()
	return items
end

local function BackButtonFunc( f3_arg0 )
	LUI.FlowManager.RequestLeaveMenu( f3_arg0 )
end

local function SettingsMenuButtons( f9_arg0 )
	f9_arg0:dispatchEventToRoot( {
		name = "add_button_helper_text",
		button_ref = "button_secondary",
		helper_text = Engine.Localize( "@LUA_MENU_BACK" ),
		side = "left",
		clickable = true,
		immediate = true
	} )
end

local function settings_menu()
	local width = GenericMenuDims.menu_right + 300 - GenericMenuDims.menu_left
	-- the rows plus the description line, for the dark panel behind them
	local height = #settings_options() * (GenericButtonSettings.Styles.GlassButton.height or 26.666666) + 28
	local menu = {
		type = "UIElement",
		id = "theater_settings_root",
		properties = {
			menu_title = TEXT.settings_title,
			rotationAllowed = false
		},
		states = {
			default = {
				topAnchor = true,
				bottomAnchor = true,
				leftAnchor = true,
				rightAnchor = true,
				top = 0,
				bottom = 0,
				left = 0,
				right = 0
			}
		},
		handlers = {
			menu_create = SettingsMenuButtons
		},
		children = {
			{
				type = "generic_menu_title",
				id = "theater_settings_title",
				properties = {
					menu_title = MBh.Property( "menu_title" )
				}
			},
			{
				-- dark panel so the values can be read over the bright parts of the menu background
				type = "UIImage",
				id = "theater_settings_panel",
				states = {
					default = {
						leftAnchor = false,
						rightAnchor = false,
						topAnchor = true,
						bottomAnchor = false,
						left = -width / 2 - 12,
						right = width / 2 + 12,
						top = GenericMenuDims.menu_top - 10,
						bottom = GenericMenuDims.menu_top + height + 2,
						material = RegisterMaterial( "white" ),
						red = 0,
						green = 0,
						blue = 0,
						alpha = 0.6
					}
				}
			},
			{
				type = "UIVerticalList",
				id = "theater_settings_list",
				childrenFeeder = SettingsFeeder,
				states = {
					default = {
						-- centered on the screen
						alignment = LUI.Alignment.Top,
						leftAnchor = false,
						rightAnchor = false,
						topAnchor = true,
						bottomAnchor = false,
						left = -width / 2,
						right = width / 2,
						top = GenericMenuDims.menu_top,
						bottom = GenericMenuDims.menu_bottom
					}
				}
			},
			{
				type = "UIBindButton",
				id = "theater_settings_bindbuttons",
				handlers = {
					button_secondary = BackButtonFunc
				}
			},
			{
				type = "button_helper_text_main",
				id = "theater_settings_button_helper_text_id"
			}
		}
	}

	-- opened from the Start menu of a film: the film behind it blurred and darkened, like the game's own options
	if not Engine.InFrontend() then
		table.insert( menu.children, 1, {
			type = "UIWorldBlur",
			id = "theater_settings_world_blur",
			states = {
				default = {
					worldBlur = 5
				}
			}
		} )
		table.insert( menu.children, 2, {
			type = "UIImage",
			id = "theater_settings_background",
			states = {
				default = {
					topAnchor = true,
					bottomAnchor = true,
					leftAnchor = true,
					rightAnchor = true,
					top = 0,
					bottom = 0,
					left = 0,
					right = 0,
					alpha = 0.5,
					material = RegisterMaterial( "black" )
				}
			}
		} )
	end

	return menu
end

LUI.MenuBuilder.registerDef( SETTINGS_MENU, settings_menu )

-- ------------------------------------------------------------------ THEATER menu (demo list)

local function DemoFeeder( f1_arg0 )
	local buttons = {}

	for _, folder in ipairs( DEMO_FOLDERS ) do
		if directoryexists( folder.path ) then
			local files = listfiles( folder.path )

			-- demos recorded as the host have a "<demo>.dm_iw6.pov" file with every player's view
			local has_pov = {}
			for _, file_path in ipairs( files ) do
				if file_path:sub( -11 ) == ".dm_iw6.pov" then
					has_pov[file_path:sub( 1, -5 )] = true
				end
			end

			for i = #files, 1, -1 do
				local file_path = files[i]
				if file_path:sub( -7 ) == ".dm_iw6" then
					local name = file_path:gsub( "^.*/", "" ):gsub( "%.dm_iw6$", "" )
					local suffix = has_pov[file_path] and "  [ALL PLAYERS]" or ""
					buttons[#buttons + 1] = {
						type = "UIGenericButton",
						id = "theater_demo_" .. #buttons,
						properties = {
							button_text = folder.tag .. name .. suffix,
							button_action_func = MBh.EmitEventToRoot( {
								name = "theater_select_demo",
								demo_path = file_path
							} )
						}
					}
				end
			end
		end
	end

	if #buttons == 0 then
		buttons[1] = {
			type = "UIGenericButton",
			id = "theater_no_demos",
			disabled = true,
			properties = {
				button_text = TEXT.no_demos
			}
		}
	end

	return buttons
end

local function SelectDemo( f2_arg0, f2_arg1 )
	Engine.Exec( "demoplay \"" .. f2_arg1.demo_path .. "\" fullpath repeat" )
end

local function OpenSettings( f10_arg0, f10_arg1 )
	if settings_available() then
		LUI.FlowManager.RequestAddMenu( f10_arg0, SETTINGS_MENU, false, f10_arg1 and f10_arg1.controller, false )
	end
end

local function TheaterMenuButtons( f4_arg0 )
	f4_arg0:dispatchEventToRoot( {
		name = "add_button_helper_text",
		button_ref = "button_secondary",
		helper_text = Engine.Localize( "@LUA_MENU_BACK" ),
		side = "left",
		clickable = true,
		immediate = true
	} )
	f4_arg0:dispatchEventToRoot( {
		name = "add_button_helper_text",
		button_ref = "button_action",
		helper_text = "Watch",
		side = "left",
		clickable = true,
		immediate = true
	} )
	if settings_available() then
		f4_arg0:dispatchEventToRoot( {
			name = "add_button_helper_text",
			button_ref = "button_alt1",
			helper_text = TEXT.settings,
			side = "left",
			clickable = true,
			immediate = true
		} )
	end
end

local function theater_menu()
	return {
		type = "UIElement",
		id = "theater_menu_root",
		properties = {
			menu_title = "THEATER",
			rotationAllowed = false
		},
		states = {
			default = {
				topAnchor = true,
				bottomAnchor = true,
				leftAnchor = true,
				rightAnchor = true,
				top = 0,
				bottom = 0,
				left = 0,
				right = 0
			}
		},
		handlers = {
			menu_create = TheaterMenuButtons,
			theater_select_demo = SelectDemo
		},
		children = {
			{
				type = "generic_menu_title",
				id = "theater_menu_title",
				properties = {
					menu_title = MBh.Property( "menu_title" )
				}
			},
			{
				type = "UIScrollingVerticalList",
				id = "theater_demo_list",
				childrenFeeder = DemoFeeder,
				properties = {
					use_arrows = true,
					use_scrollbar = true,
					bar_width = 20
				},
				states = {
					default = {
						leftAnchor = true,
						rightAnchor = false,
						topAnchor = true,
						bottomAnchor = false,
						left = GenericMenuDims.menu_left,
						right = GenericMenuDims.menu_right + 160,
						top = GenericMenuDims.menu_top,
						bottom = GenericMenuDims.menu_top + GenericButtonSettings.Styles.GradientButton.height * 11 + 2 + 60
					}
				}
			},
			{
				type = "UIBindButton",
				id = "theater_menu_bindbuttons",
				handlers = {
					button_secondary = BackButtonFunc,
					button_alt1 = OpenSettings
				}
			},
			{
				type = "button_helper_text_main",
				id = "theater_menu_button_helper_text_id"
			}
		}
	}
end

LUI.MenuBuilder.registerDef( "theater_menu", theater_menu )

-- add the THEATER button to the multiplayer main menu (right after MODS when present)
local original_main_menu = LUI.MenuBuilder.m_definitions[ "main_mp_menu_options" ]
if original_main_menu then
	LUI.MenuBuilder.m_definitions[ "main_mp_menu_options" ] = function ( ... )
		local definition = original_main_menu( ... )
		local original_feeder = definition and definition.childrenFeeder
		if type( original_feeder ) == "function" then
			definition.childrenFeeder = function ( f5_arg0 )
				local buttons = original_feeder( f5_arg0 )
				local insert_at = nil
				for i = 1, #buttons do
					if buttons[i].id == "btn_MPMain_10" then
						insert_at = i + 1
					end
				end
				if not insert_at then
					insert_at = math.max( 1, #buttons - 1 )
				end
				table.insert( buttons, insert_at, {
					type = "UIGenericButton",
					id = "btn_MPMain_theater",
					properties = {
						button_text = "THEATER",
						desc_text = "Watch your demos with free camera, dolly camera and slow motion",
						button_action_func = function ( f6_arg0, f6_arg1 )
							LUI.FlowManager.RequestAddMenu( f6_arg0, "theater_menu", false, f6_arg1.controller, false )
						end,
						button_over_func = function ( f7_arg0, f7_arg1 )
							PersistentBackground.SetToDefault()
						end
					}
				} )
				return buttons
			end
		end
		return definition
	end
end

-- ------------------------------------------------------------------ private match lobby

-- GAME RECORDING at the bottom of the lobby's list, like the Plutonium Black Ops II lobby.
-- Everyone gets it, not only the host: each player records their own demo.
local private_lobby = package.loaded[ "LUI.mp_menus.MPLivePrivateLobby" ]
if Engine.InFrontend() and private_lobby and type( private_lobby.LivePrivateLobbyOptionsFeeder ) == "function" then
	local lobby_options_feeder = private_lobby.LivePrivateLobbyOptionsFeeder
	private_lobby.LivePrivateLobbyOptionsFeeder = function ( f11_arg0 )
		local items = lobby_options_feeder( f11_arg0 )

		local ok, err = pcall( function ()
			if (f11_arg0 and f11_arg0.aliensSoloMode == true) or Engine.IsAliensMode() or not dvar_exists( DVARS.game_recording, Engine.GetDvarBool ) then
				return
			end

			-- last of the buttons (before the description line when the list ends with it)
			local index = #items + 1
			if index > 1 and items[index - 1].type == "button_desc_text" then
				index = index - 1
			end

			-- the lobby's own style, as wide as the FRIENDLY BOTS row (no slide on focus). Like that row it turns the
			-- lobby's Right = "slide to the LOBBY LIST" off while it has the focus, so Left / Right only change the value
			local button = toggle_option( DVARS.game_recording, {
				button_text = TEXT.game_recording,
				desc_text = TEXT.game_recording_desc,
				focus_animation_distance = 0,
				button_over_func = MBh.EmitEventToRoot( {
					name = "lobby_slide_disable",
					dispatchChildren = true,
					immediate = true
				} )
			} )
			table.insert( items, index, button )
		end )

		if not ok then
			print( "theater: " .. tostring( err ) )
		end

		return items
	end
end

-- ------------------------------------------------------------------ Start menu of a film

-- THEATER SETTINGS right after OPTIONS in the Start (pause) menu while a film plays (the add-on keeps theater_film at 1).
-- CBServers builds that menu's list in LUI.mp_hud.OptionsMenu.options_def (data\ui_scripts\team_select), loaded first.
local pause_menu = package.loaded[ "LUI.mp_hud.OptionsMenu" ]
if not Engine.InFrontend() and pause_menu and type( pause_menu.options_def ) == "function" then
	local pause_options_def = pause_menu.options_def
	pause_menu.options_def = function ( ... )
		local list = pause_options_def( ... )

		local ok, err = pcall( function ()
			if not list or not Engine.GetDvarBool( DVARS.film ) or not settings_available() then
				return
			end

			LUI.MenuBuilder.BuildAddChild( list, {
				type = "UIGenericButton",
				id = "btn_MPPause_theater",
				properties = {
					childNum = 8,
					button_text = TEXT.settings_title,
					button_action_func = function ( f19_arg0, f19_arg1 )
						LUI.FlowManager.RequestAddMenu( f19_arg0, SETTINGS_MENU, false, f19_arg1 and f19_arg1.controller, false )
					end
				}
			} )

			-- BuildAddChild adds it at the end: move it after OPTIONS
			local button = list:getChildById( "btn_MPPause_theater" )
			local options_button = list:getChildById( "btn_MPPause_2" )
			if button and options_button then
				list:removeElement( button )
				button:addElementAfter( options_button )
			end
		end )

		if not ok then
			print( "theater: " .. tostring( err ) )
		end

		return list
	end
end

-- ------------------------------------------------------------------ feed of a film

-- A seek leaves on screen the kills and messages the feed was showing, which belong to the moment the film left (a
-- video rendered with the HUD on began with them). The add-on sends "theater_feed_clear" when a seek starts and the
-- feed closes its lines. The lines of the part of the film the seek skips never get here (the add-on drops them).
local hud_types = LUI.MenuBuilder.m_types
local feed_def = hud_types and hud_types[ "obituaryHudDef" ]
if not Engine.InFrontend() and type( feed_def ) == "function" then
	hud_types[ "obituaryHudDef" ] = function ( ... )
		local feed = feed_def( ... )

		local ok, err = pcall( function ()
			feed:registerEventHandler( "theater_feed_clear", function ( element, event )
				local list = element:getFirstDescendentById( "obituaryVLId" )
				if list then
					list:closeChildren()
				end
			end )
		end )

		if not ok then
			print( "theater: " .. tostring( err ) )
		end

		return feed
	end
end
