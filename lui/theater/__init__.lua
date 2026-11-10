-- iw6-theater: "THEATER" entry in the multiplayer main menu with a list of recorded demos.
-- Installed to <game>\iw6\ui_scripts\theater\__init__.lua (outside the launcher-managed data\ folder).
-- Demo playback, cameras and the timeline are handled by the native add-on (XINPUT1_3.dll).

local DEMO_FOLDERS = {
	{ path = "demos/client/user/", tag = "" },
	{ path = "demos/client/auto/", tag = "[auto] " },
	{ path = "demos/server/", tag = "[server] " },
}

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
				button_text = "NO DEMOS YET - RECORD WITH demostart / demostop"
			}
		}
	end

	return buttons
end

local function SelectDemo( f2_arg0, f2_arg1 )
	Engine.Exec( "demoplay \"" .. f2_arg1.demo_path .. "\" fullpath repeat" )
end

local function BackButtonFunc( f3_arg0 )
	LUI.FlowManager.RequestLeaveMenu( f3_arg0 )
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
					button_secondary = BackButtonFunc
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
