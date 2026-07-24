local tab
local pulse_generation = 0

local function publish_summary(primary, secondary, severity)
    tab:update_summary({
        primary_text = primary,
        secondary_text = secondary,
        value = 2,
        units = "checks",
        severity = severity,
    })
end

local function pulse(generation, step)
    if generation ~= pulse_generation or not tab:is_valid() then
        return
    end

    tab:update_controls({
        {
            id = "health",
            text = "Pulse " .. step,
            progress = step / 20,
        },
        {
            id = "pulse",
            pending = step < 20,
        },
    })
    publish_summary(
        "Ready",
        step < 20 and ("Background pulse " .. step) or "Pulse complete",
        step < 20 and "active" or "success"
    )

    if step < 20 then
        c2.later(function()
            pulse(generation, step + 1)
        end, 50)
    end
end

local function on_control(id, proposed)
    if id == "pulse" then
        pulse_generation = pulse_generation + 1
        tab:update_controls({ { id = "pulse", pending = true } })
        pulse(pulse_generation, 1)
    elseif id == "notifications" then
        tab:update_controls({ { id = id, value = proposed } })
        publish_summary(
            proposed and "Notifications on" or "Notifications off",
            "Toggle confirmed by the example plugin",
            proposed and "success" or "neutral"
        )
    elseif id == "profile" then
        tab:update_controls({ { id = id, value = proposed } })
        publish_summary(
            "Profile " .. proposed,
            "Choice confirmed by the example plugin",
            "success"
        )
    end
end

tab = c2.ui.register_control_tab({
    title = "Harness",
    icon = "settings",
    tooltip = "Generic plugin control-tab demonstration",
    accessible_label = "Control-tab example",
    summary = {
        primary_text = "Ready",
        secondary_text = "All systems nominal",
        value = 2,
        units = "checks",
        severity = "success",
    },
    controls = {
        {
            id = "health",
            type = "status",
            label = "Health",
            text = "All systems nominal",
            progress = 0,
        },
        {
            id = "pulse",
            type = "action",
            label = "Pulse status",
            icon = "refresh",
        },
        {
            id = "notifications",
            type = "toggle",
            label = "Notifications",
            value = false,
        },
        {
            id = "profile",
            type = "choice",
            label = "Profile",
            value = "safe",
            options = {
                { value = "safe", label = "Safe" },
                { value = "fast", label = "Fast" },
            },
        },
    },
    on_control = on_control,
})
