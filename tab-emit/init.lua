-- Chatterino Tab Emit v0.11.0
-- Emits selected Chatterino tab/channel state to a local WebSocket server.

local WS_URL = "ws://127.0.0.1:8765"
local RECONNECT_DELAY_MS = 1000
local AUTO_CONTEXT_LIMIT = 100

local ws = nil
local connected = false
local reconnect_scheduled = false
local sent_count = 0
local tick_count = 0
local last_error = nil
local last_identity = nil
local command_results = {}
local windows_checked = false
local windows_available = true
local state_error_sent = false

local function esc_char(c)
    if c == '"' then return '\\"' end
    if c == '\\' then return '\\\\' end
    if c == '\b' then return '\\b' end
    if c == '\f' then return '\\f' end
    if c == '\n' then return '\\n' end
    if c == '\r' then return '\\r' end
    if c == '\t' then return '\\t' end
    return string.format("\\u%04x", string.byte(c))
end

local function js(v)
    if v == nil then return "null" end
    local s = tostring(v):gsub('[%z\1-\31\\"]', esc_char)
    return '"' .. s .. '"'
end

local function jb(v) return v and "true" or "false" end
local function jbn(v)
    if type(v) ~= "boolean" then return "null" end
    return jb(v)
end
local function jn(v) return type(v) == "number" and tostring(v) or "null" end

local function jarr(t)
    local o = {}
    if t then for _, v in ipairs(t) do o[#o + 1] = js(v) end end
    return "[" .. table.concat(o, ",") .. "]"
end

local function command_json()
    local o = {}
    for _, r in ipairs(command_results) do
        o[#o + 1] = "{\"name\":" .. js(r.name) .. ",\"ok\":" .. jb(r.ok) .. "}"
    end
    return "[" .. table.concat(o, ",") .. "]"
end

local function fallback_state()
    return {
        page_index = nil,
        channel = nil,
        display_name = nil,
        channel_type = nil,
        platform = nil,
        channel_object = nil,
        stream_url = nil,
        is_live = nil,
        identity = nil,
        all_channels = {},
        state_ok = false,
    }
end

local function try_call(obj, method)
    if obj == nil then return false, nil end
    return pcall(function() return obj[method](obj) end)
end

local function try_prop(obj, prop)
    if obj == nil then return false, nil end
    return pcall(function() return obj[prop] end)
end

local function lower_or_empty(v)
    if v == nil then return "" end
    return string.lower(tostring(v))
end

local function normalize_platform(v)
    local p = lower_or_empty(v)
    if p == "kick" then return "kick" end
    if p == "twitch" then return "twitch" end
    if p == "rumble" then return "rumble" end
    return nil
end

local function is_rumble_type(v)
    local ok_enum, enum_value = pcall(function()
        return c2 and c2.ChannelType and c2.ChannelType.Rumble
    end)
    if ok_enum and enum_value ~= nil and v == enum_value then return true end
    return string.find(lower_or_empty(v), "rumble", 1, true) ~= nil
end

local function platform_hint(info)
    if info == nil then return "" end
    return table.concat({
        lower_or_empty(info.platform),
        lower_or_empty(info.type),
        lower_or_empty(info.lua_type),
        lower_or_empty(info.object_id),
        lower_or_empty(info.display_name),
    }, " ")
end

local function stream_url_for(info)
    if info == nil or info.name == nil then return nil end
    if info.platform == "rumble" then
        return tostring(info.name)
    end
    if info.platform == "kick" then
        return "https://kick.com/" .. tostring(info.name)
    end
    if info.platform == "twitch" then
        return "https://www.twitch.tv/" .. tostring(info.name)
    end

    local hint = platform_hint(info)
    if string.find(hint, "kick", 1, true) then
        return "https://kick.com/" .. tostring(info.name)
    end
    if string.find(hint, "twitch", 1, true) then
        return "https://www.twitch.tv/" .. tostring(info.name)
    end
    return nil
end

local function channel_info(ch, platform_override, locator_override, live_override)
    if ch == nil then return nil end
    local ok_valid, valid = try_call(ch, "is_valid")
    local out = { lua_type = type(ch), valid_ok = ok_valid, valid = valid }
    out.platform = normalize_platform(platform_override)
    if type(live_override) == "boolean" then out.is_live = live_override end

    local ok_type, typ = try_call(ch, "get_type")
    if ok_type and typ ~= nil then
        out.type = tostring(typ)
        if out.platform == nil and is_rumble_type(typ) then
            out.platform = "rumble"
        end
    end

    -- Rumble runtime names are opaque UUID-like implementation identities.
    -- Only the revalidated public locator exposed by c2.Split may identify a
    -- selected Rumble channel. Keep display metadata separate and never even
    -- stringify the runtime object on this path.
    if out.platform == "rumble" then
        if locator_override ~= nil and tostring(locator_override) ~= "" then
            out.name = tostring(locator_override)
        end
        local ok_display, display = try_call(ch, "get_display_name")
        if ok_display and display ~= nil then out.display_name = display end
        if out.display_name == nil then
            local ok_prop_display, prop_display = try_prop(ch, "display_name")
            if ok_prop_display and prop_display ~= nil then out.display_name = prop_display end
        end
        out.stream_url = out.name
        return out
    end

    out.object_id = tostring(ch)

    local ok_name, name = try_call(ch, "get_name")
    if ok_name and name ~= nil then out.name = name end

    local ok_display, display = try_call(ch, "get_display_name")
    if ok_display and display ~= nil then out.display_name = display end

    if out.name == nil then
        local ok_prop_name, prop_name = try_prop(ch, "name")
        if ok_prop_name and prop_name ~= nil then out.name = prop_name end
    end
    if out.display_name == nil then
        local ok_prop_display, prop_display = try_prop(ch, "display_name")
        if ok_prop_display and prop_display ~= nil then out.display_name = prop_display end
    end

    out.stream_url = stream_url_for(out)
    return out
end

local function split_channel_info(split)
    if split == nil then return nil end

    local platform = nil
    local ok_platform, selected_platform = pcall(function() return split.selected_platform end)
    if ok_platform then platform = normalize_platform(selected_platform) end

    local locator = nil
    local ok_locator, selected_locator = pcall(function() return split.selected_locator end)
    if ok_locator and selected_locator ~= nil and tostring(selected_locator) ~= "" then
        locator = tostring(selected_locator)
    end

    local is_live = nil
    if platform == "rumble" then
        local ok_live, selected_is_live = pcall(function() return split.selected_is_live end)
        if ok_live and type(selected_is_live) == "boolean" then is_live = selected_is_live end
    end

    local ok_selected, selected = pcall(function() return split.selected_channel end)
    if ok_selected and selected ~= nil then
        local selected_info = channel_info(selected, platform, locator, is_live)
        if platform == "rumble" then return selected_info end
        if selected_info ~= nil and selected_info.name ~= nil then return selected_info end
    end

    local ok, ch = pcall(function() return split.channel end)
    if not ok then return nil end
    return channel_info(ch, platform, nil, is_live)
end

local function ctx_keys(ctx)
    local keys = {}
    if ctx == nil then return keys, false end
    local ok = pcall(function()
        for k, _ in pairs(ctx) do keys[#keys + 1] = tostring(k) end
    end)
    return keys, ok
end

local function state_from_ctx(ctx)
    local st = fallback_state()
    local ok_channel, ch = pcall(function() return ctx and ctx.channel end)
    if not ok_channel or ch == nil then
        st.identity = "ctx:no-channel"
        return st
    end

    local info = channel_info(ch)
    if info == nil then
        st.identity = "ctx:nil-channel-info"
        return st
    end

    local names = {}
    if info.name ~= nil then names[1] = info.name end

    st.channel = info.name
    st.display_name = info.display_name
    st.channel_type = info.type or info.lua_type
    st.platform = info.platform
    st.channel_object = info.object_id
    st.stream_url = info.stream_url
    st.all_channels = names
    st.identity = "ctx:" .. tostring(info.platform or "") .. ":" .. tostring(info.type or info.lua_type or "unknown") .. ":" .. tostring(info.object_id or "unknown") .. ":" .. tostring(info.name or "unknown")
    st.state_ok = info.name ~= nil
    return st
end

local function selected_page_index(notebook, selected_page)
    local ok_count, count = pcall(function() return notebook.page_count end)
    if not ok_count or type(count) ~= "number" then return nil end
    local selected_key = tostring(selected_page)
    for i = 0, count - 1 do
        local ok_page, page = pcall(function() return notebook:page_at(i) end)
        if ok_page and page ~= nil and (page == selected_page or tostring(page) == selected_key) then
            return i
        end
    end
    return nil
end

local function get_window()
    windows_checked = true
    local ok_windows, windows = pcall(function() return c2.windows end)
    if not ok_windows then
        windows_available = false
        return nil, "could not read c2.windows"
    end
    if windows == nil then
        windows_available = false
        return nil, "c2.windows is nil"
    end

    local ok_last, last = pcall(function() return windows.last_selected_window end)
    if ok_last and last ~= nil then return last, nil end

    local ok_main, main = pcall(function() return windows.main_window end)
    if ok_main and main ~= nil then return main, nil end

    local ok_all, all = pcall(function() return windows:all() end)
    if ok_all and all ~= nil and all[1] ~= nil then return all[1], nil end

    return nil, "no readable Chatterino window"
end

local function auto_context(page)
    local ok_sel, selected_split = pcall(function() return page.selected_split end)
    if not ok_sel or selected_split == nil then return false end
    local ok_changed, changed = pcall(function()
        return selected_split:auto_select_context_by_recent_messages(AUTO_CONTEXT_LIMIT)
    end)
    return ok_changed and changed
end

local function auto_state()
    local win, err = get_window()
    if win == nil then
        last_error = err
        return nil, err
    end

    local ok_notebook, notebook = pcall(function() return win.notebook end)
    if not ok_notebook or notebook == nil then
        last_error = "could not read window.notebook"
        return nil, last_error
    end

    local ok_page, page = pcall(function() return notebook.selected_page end)
    if not ok_page or page == nil then
        last_error = "could not read notebook.selected_page"
        return nil, last_error
    end

    auto_context(page)

    local page_index = selected_page_index(notebook, page)
    local names = {}
    local ok_splits, splits = pcall(function() return page:splits() end)
    if ok_splits and splits ~= nil then
        for _, split in ipairs(splits) do
            local info = split_channel_info(split)
            if info ~= nil and info.name ~= nil then names[#names + 1] = info.name end
        end
    end

    local selected_info = nil
    local ok_sel, selected_split = pcall(function() return page.selected_split end)
    if ok_sel then selected_info = split_channel_info(selected_split) end

    local selected_name = nil
    if selected_info ~= nil then
        selected_name = selected_info.name
    else
        selected_name = names[1]
    end
    local selected_type = selected_info and selected_info.type or nil
    local selected_platform = selected_info and selected_info.platform or nil
    local selected_object = selected_info and selected_info.object_id or nil
    local selected_url = selected_info and selected_info.stream_url or nil
    local selected_live = nil
    if selected_info ~= nil then selected_live = selected_info.is_live end
    local identity
    if page_index ~= nil then
        identity = "idx:" .. tostring(page_index) .. ":" .. tostring(selected_platform or "") .. ":" .. tostring(selected_type or "") .. ":" .. tostring(selected_object or "") .. ":" .. tostring(selected_name or "") .. ":" .. tostring(selected_url or "") .. ":" .. tostring(selected_info and selected_info.is_live) .. ":" .. table.concat(names, "|")
    else
        identity = "channels:" .. table.concat(names, "|") .. ":selected:" .. tostring(selected_platform or "") .. ":" .. tostring(selected_type or "") .. ":" .. tostring(selected_object or "") .. ":" .. tostring(selected_name or "") .. ":" .. tostring(selected_url or "") .. ":" .. tostring(selected_info and selected_info.is_live)
    end

    return {
        page_index = page_index,
        channel = selected_name,
        display_name = selected_info and selected_info.display_name or nil,
        channel_type = selected_info and selected_info.type or nil,
        platform = selected_platform,
        channel_object = selected_object,
        stream_url = selected_url,
        is_live = selected_live,
        all_channels = names,
        identity = identity,
        state_ok = selected_name ~= nil,
    }, nil
end

local function encode_event(event, state, reason)
    state = state or fallback_state()
    return "{"
        .. "\"event\":" .. js(event) .. ","
        .. "\"reason\":" .. js(reason) .. ","
        .. "\"page_index\":" .. jn(state.page_index) .. ","
        .. "\"channel\":" .. js(state.channel) .. ","
        .. "\"display_name\":" .. js(state.display_name) .. ","
        .. "\"channel_type\":" .. js(state.channel_type) .. ","
        .. "\"platform\":" .. js(state.platform) .. ","
        .. "\"channel_object\":" .. js(state.channel_object) .. ","
        .. "\"stream_url\":" .. js(state.stream_url) .. ","
        .. "\"is_live\":" .. jbn(state.is_live) .. ","
        .. "\"identity\":" .. js(state.identity) .. ","
        .. "\"all_channels\":" .. jarr(state.all_channels) .. ","
        .. "\"state_ok\":" .. jb(state.state_ok) .. ","
        .. "\"connected\":" .. jb(connected) .. ","
        .. "\"sent_count\":" .. tostring(sent_count) .. ","
        .. "\"tick_count\":" .. tostring(tick_count) .. ","
        .. "\"last_error\":" .. js(last_error) .. ","
        .. "\"windows_checked\":" .. jb(windows_checked) .. ","
        .. "\"windows_available\":" .. jb(windows_available) .. ","
        .. "\"commands\":" .. command_json()
        .. "}"
end

local function send_raw(payload)
    if ws == nil or not connected then return false end
    local ok, err = pcall(function() ws:send_text(payload) end)
    if ok then
        sent_count = sent_count + 1
        return true
    end
    connected = false
    last_error = tostring(err)
    return false
end

local function send_event(event, state, reason)
    return send_raw(encode_event(event, state, reason))
end

local function check_auto(reason, force)
    if windows_checked and not windows_available then return end

    local st, err = auto_state()
    if st == nil then
        if force or not state_error_sent then
            state_error_sent = true
            send_event("state_error", fallback_state(), reason or err)
        end
        return
    end

    state_error_sent = false
    if force or st.identity ~= last_identity then
        local first = last_identity == nil
        last_identity = st.identity
        send_event(first and "initial_state" or "tab_changed", st, reason)
    end
end

local function api_probe()
    local keys = {}
    local ok_pairs = pcall(function()
        for k, _ in pairs(c2) do keys[#keys + 1] = tostring(k) end
    end)
    if not ok_pairs then keys = { "<pairs(c2) failed>" } end

    local ok_windows, windows = pcall(function() return c2.windows end)
    if ok_windows and windows == nil then windows_available = false end
    local ok_main, main = pcall(function() return windows and windows.main_window end)
    local ok_last, last = pcall(function() return windows and windows.last_selected_window end)
    send_raw("{"
        .. "\"event\":\"api_probe\"," 
        .. "\"c2_type\":" .. js(type(c2)) .. ","
        .. "\"c2_keys\":" .. jarr(keys) .. ","
        .. "\"c2_windows_ok\":" .. jb(ok_windows) .. ","
        .. "\"c2_windows_type\":" .. js(type(windows)) .. ","
        .. "\"main_window_ok\":" .. jb(ok_main) .. ","
        .. "\"main_window_type\":" .. js(type(main)) .. ","
        .. "\"last_selected_window_ok\":" .. jb(ok_last) .. ","
        .. "\"last_selected_window_type\":" .. js(type(last)) .. ","
        .. "\"last_error\":" .. js(last_error)
        .. "}")
end

local function ctx_probe(ctx, label)
    local keys, keys_ok = ctx_keys(ctx)
    local ok_channel, ch = pcall(function() return ctx and ctx.channel end)
    local info = nil
    if ok_channel then info = channel_info(ch) end
    local st = state_from_ctx(ctx)
    send_raw("{"
        .. "\"event\":\"ctx_probe\"," 
        .. "\"label\":" .. js(label) .. ","
        .. "\"ctx_type\":" .. js(type(ctx)) .. ","
        .. "\"ctx_keys_ok\":" .. jb(keys_ok) .. ","
        .. "\"ctx_keys\":" .. jarr(keys) .. ","
        .. "\"ctx_channel_ok\":" .. jb(ok_channel) .. ","
        .. "\"ctx_channel_type\":" .. js(type(ch)) .. ","
        .. "\"channel_valid_ok\":" .. jb(info and info.valid_ok) .. ","
        .. "\"channel_valid\":" .. jb(info and info.valid) .. ","
        .. "\"channel_name\":" .. js(info and info.name) .. ","
        .. "\"channel_display_name\":" .. js(info and info.display_name) .. ","
        .. "\"channel_type\":" .. js(info and info.type) .. ","
        .. "\"platform\":" .. js(info and info.platform) .. ","
        .. "\"channel_object\":" .. js(info and info.object_id) .. ","
        .. "\"stream_url\":" .. js(info and info.stream_url) .. ","
        .. "\"state_identity\":" .. js(st.identity) .. ","
        .. "\"state_ok\":" .. jb(st.state_ok) .. ","
        .. "\"last_error\":" .. js(last_error)
        .. "}")
    send_event("manual_check_ctx", st, label)
end

local function add_msg(ctx, text)
    pcall(function()
        if ctx and ctx.channel then ctx.channel:add_system_message("tab-emit: " .. text) end
    end)
end

local function switch_ctx(ctx, label)
    local st = state_from_ctx(ctx)
    send_event("manual_check_ctx", st, label)
    add_msg(ctx, "switch fired; connected=" .. tostring(connected) .. " error=" .. tostring(last_error))
end

local reconnect
local function schedule_reconnect(reason)
    if connected or reconnect_scheduled then return end
    reconnect_scheduled = true
    local ok, err = pcall(function()
        c2.later(function()
            reconnect_scheduled = false
            if not connected then reconnect(false) end
            if not connected then schedule_reconnect("retry") end
        end, RECONNECT_DELAY_MS)
    end)
    if not ok then
        last_error = "c2.later failed after " .. tostring(reason) .. ": " .. tostring(err)
        reconnect_scheduled = false
    end
end

reconnect = function(requested)
    connected = false
    local ok, result = pcall(function()
        return c2.WebSocket.new(WS_URL, {
            on_open = function()
                connected = true
                reconnect_scheduled = false
                last_error = nil
                send_event("hello", fallback_state(), "open")
                api_probe()
                check_auto("open", true)
            end,
            on_text = function(text)
                if text == "tick" then
                    tick_count = tick_count + 1
                    check_auto("tick", false)
                elseif text == "check" then
                    check_auto("listener_check", true)
                elseif text == "ping" then
                    send_event("plugin_pong", fallback_state(), "listener_ping")
                elseif text == "probe" then
                    api_probe()
                end
            end,
            on_close = function()
                connected = false
                last_error = "websocket closed"
                schedule_reconnect("close")
            end,
        })
    end)

    if ok then
        ws = result
    else
        last_error = "WebSocket.new failed: " .. tostring(result)
        schedule_reconnect(requested and "requested-failed" or "initial-failed")
    end
end

local function reg(name, fn)
    local ok = pcall(function() c2.register_command(name, fn) end)
    command_results[#command_results + 1] = { name = name, ok = ok }
end

reg("/tabemit-status", function(ctx)
    add_msg(ctx, "connected=" .. tostring(connected) .. " sent=" .. tostring(sent_count) .. " ticks=" .. tostring(tick_count) .. " error=" .. tostring(last_error))
    send_event("status", fallback_state(), "command")
end)
reg("tabemit-status", function(ctx)
    add_msg(ctx, "connected=" .. tostring(connected) .. " sent=" .. tostring(sent_count) .. " ticks=" .. tostring(tick_count) .. " error=" .. tostring(last_error))
    send_event("status", fallback_state(), "command-noslash")
end)

reg("/tabemit-switch", function(ctx)
    switch_ctx(ctx, "switch-command")
end)
reg("tabemit-switch", function(ctx)
    switch_ctx(ctx, "switch-bare-command")
end)

reg("/tabemit-check", function(ctx)
    ctx_probe(ctx, "slash-command")
    check_auto("command", true)
    add_msg(ctx, "check fired; connected=" .. tostring(connected) .. " error=" .. tostring(last_error))
end)
reg("tabemit-check", function(ctx)
    ctx_probe(ctx, "bare-command")
    check_auto("command", true)
    add_msg(ctx, "check fired; connected=" .. tostring(connected) .. " error=" .. tostring(last_error))
end)

reg("/tabemit-ping", function(ctx)
    ctx_probe(ctx, "ping-command")
    send_event("plugin_pong", state_from_ctx(ctx), "command")
    add_msg(ctx, "pong fired; connected=" .. tostring(connected))
end)
reg("tabemit-ping", function(ctx)
    ctx_probe(ctx, "ping-bare-command")
    send_event("plugin_pong", state_from_ctx(ctx), "command-noslash")
end)

reg("/tabemit-probe", function(ctx)
    api_probe()
    ctx_probe(ctx, "probe-command")
    add_msg(ctx, "probe fired; c2.windows error=" .. tostring(last_error))
end)
reg("tabemit-probe", function(ctx)
    api_probe()
    ctx_probe(ctx, "probe-bare-command")
end)

reg("/tabemit-reconnect", function(ctx)
    reconnect(true)
    add_msg(ctx, "reconnect fired; connected=" .. tostring(connected) .. " error=" .. tostring(last_error))
end)
reg("tabemit-reconnect", function(ctx) reconnect(true) end)

reconnect(false)
