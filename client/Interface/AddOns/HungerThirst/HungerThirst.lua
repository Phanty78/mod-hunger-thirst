--[[--------------------------------------------------------------------------
    Hunger & Thirst - hunger, thirst and cold gauges (client 3.3.5a, Interface 30300).

    Server protocol, addon prefix "HT":
        HT<TAB>satiety<TAB>hydration<TAB>mode<TAB>lethal<TAB>loaded
          <TAB>persistence<TAB>satBand<TAB>hydBand
          <TAB>cold<TAB>coldBand<TAB>hot

    The eleven fields are, in order: 1 satiety, 2 hydration, 3 mode, 4 lethal,
    5 loaded, 6 persistence, 7 satBand, 8 hydBand, 9 cold, 10 coldBand, 11 hot.

    Fields 9-11 are optional and were appended to the original eight-field
    protocol: older addons ignore them, and a message that only carries eight
    fields leaves cold at 0 (comfortable) and hot at false.

    mode: 0 = disabled, 1 = advisory, 2 = survival
    band: 0 = normal, 1 = warning, 2 = critical, 3 = empty
    cold band: 0 = Comfortable, 1 = Chilly, 2 = Cold, 3 = Freezing
    cold: 0 = comfortable, 100 = freezing
    hot: 0 = temperate or cold zone, 1 = hot zone (hydration drains faster)

    The prefix is sent separately through CHAT_MSG_ADDON; the leading "HT"
    token in the message body is accepted and then ignored.

    A bar of three status icons stays visible at all times, even when the main
    frame is hidden; "/hungerthirst icons" shows or hides it.
----------------------------------------------------------------------------]]

local ADDON_PREFIX = "HT"
local MAX_VALUE = 100
local DEFAULT_POSITION = { point = "CENTER", relativePoint = "CENTER", x = -160, y = -140 }

local MODE_LABELS = {
    [0] = "Disabled",
    [1] = "Advisory",
    [2] = "Survival",
}

-- Threshold colors: normal, yellow, orange, red.
local BAND_COLORS = {
    [0] = { 0.35, 1.00, 0.40 },
    [1] = { 1.00, 0.90, 0.25 },
    [2] = { 1.00, 0.55, 0.10 },
    [3] = { 1.00, 0.20, 0.20 },
}

-- Identity colors of the three gauges, used while the band is normal.
local BAR_COLORS = {
    satiety   = { 1.00, 0.62, 0.12 },
    hydration = { 0.20, 0.58, 1.00 },
    cold      = { 0.55, 0.85, 1.00 },
}

local BAND_LABELS = {
    satiety   = { [0] = "Satiated", [1] = "Hungry", [2] = "Very hungry", [3] = "Starving" },
    hydration = { [0] = "Hydrated", [1] = "Thirsty", [2] = "Very thirsty", [3] = "Dehydrated" },
    cold      = { [0] = "Comfortable", [1] = "Chilly", [2] = "Cold", [3] = "Freezing" },
}

local State = {
    satiety = MAX_VALUE,
    hydration = MAX_VALUE,
    mode = 0,
    lethal = false,
    loaded = false,
    persistence = false,
    satietyBand = 0,
    hydrationBand = 0,
    cold = 0,
    coldBand = 0,
    hot = false,
}

if type(HungerThirstDB) ~= "table" then
    HungerThirstDB = {}
end

local function Clamp(value)
    if value < 0 then
        return 0
    end
    if value > MAX_VALUE then
        return MAX_VALUE
    end
    return value
end

local function NormalizeBand(band)
    band = math.floor(tonumber(band) or 0)
    if band < 0 or band > 3 then
        return 0
    end
    return band
end

local function NormalizeMode(mode)
    mode = math.floor(tonumber(mode) or 0)
    if mode < 0 or mode > 2 then
        return 0
    end
    return mode
end

local function BandColor(band)
    local color = BAND_COLORS[band] or BAND_COLORS[0]
    return color[1], color[2], color[3]
end

local function BarFillColor(key, band)
    if band == 0 then
        local color = BAR_COLORS[key]
        return color[1], color[2], color[3]
    end
    return BandColor(band)
end

--------------------------------------------------------------------------------
-- Frame
--------------------------------------------------------------------------------

local Frame = CreateFrame("Frame", "HungerThirstFrame", UIParent)
Frame:SetWidth(240)
Frame:SetHeight(192)
Frame:SetFrameStrata("MEDIUM")
Frame:SetClampedToScreen(true)
Frame:SetMovable(true)
Frame:EnableMouse(true)
Frame:RegisterForDrag("LeftButton")
Frame:SetBackdrop({
    bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true,
    tileSize = 16,
    edgeSize = 16,
    insets = { left = 4, right = 4, top = 4, bottom = 4 },
})
Frame:SetBackdropColor(0.05, 0.05, 0.05, 0.78)
Frame:SetBackdropBorderColor(0.55, 0.45, 0.25, 1)

local Title = Frame:CreateFontString(nil, "OVERLAY", "GameFontNormal")
Title:SetPoint("TOP", Frame, "TOP", 0, -7)
Title:SetText("Hunger & Thirst")
Title:SetTextColor(1.00, 0.82, 0.25)

local function CreateGauge(parent, name, key)
    local gauge = { name = name, key = key }

    gauge.label = parent:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    gauge.label:SetWidth(168)
    gauge.label:SetJustifyH("LEFT")
    gauge.label:SetText(name)

    gauge.value = parent:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    gauge.value:SetJustifyH("RIGHT")
    gauge.value:SetText("--")

    gauge.bar = CreateFrame("StatusBar", nil, parent)
    gauge.bar:SetHeight(13)
    gauge.bar:SetMinMaxValues(0, MAX_VALUE)
    gauge.bar:SetValue(0)
    gauge.bar:SetStatusBarTexture("Interface\\TargetingFrame\\UI-StatusBar")
    gauge.bar:SetBackdrop({
        bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 8,
        edgeSize = 10,
        insets = { left = 1, right = 1, top = 1, bottom = 1 },
    })
    gauge.bar:SetBackdropColor(0, 0, 0, 0.60)
    gauge.bar:SetBackdropBorderColor(0.30, 0.30, 0.30, 1)

    return gauge
end

local SatietyGauge = CreateGauge(Frame, "Satiety", "satiety")
SatietyGauge.label:SetPoint("TOPLEFT", Frame, "TOPLEFT", 12, -26)
SatietyGauge.value:SetPoint("TOPRIGHT", Frame, "TOPRIGHT", -12, -26)
SatietyGauge.bar:SetPoint("TOPLEFT", SatietyGauge.label, "BOTTOMLEFT", 0, -3)
SatietyGauge.bar:SetPoint("TOPRIGHT", SatietyGauge.value, "BOTTOMRIGHT", 0, -3)

local HydrationGauge = CreateGauge(Frame, "Hydration", "hydration")
HydrationGauge.label:SetPoint("TOPLEFT", SatietyGauge.bar, "BOTTOMLEFT", 0, -10)
HydrationGauge.value:SetPoint("TOPRIGHT", SatietyGauge.bar, "BOTTOMRIGHT", 0, -10)
HydrationGauge.bar:SetPoint("TOPLEFT", HydrationGauge.label, "BOTTOMLEFT", 0, -3)
HydrationGauge.bar:SetPoint("TOPRIGHT", HydrationGauge.value, "BOTTOMRIGHT", 0, -3)

local ColdGauge = CreateGauge(Frame, "Cold", "cold")
ColdGauge.label:SetPoint("TOPLEFT", HydrationGauge.bar, "BOTTOMLEFT", 0, -10)
ColdGauge.value:SetPoint("TOPRIGHT", HydrationGauge.bar, "BOTTOMRIGHT", 0, -10)
ColdGauge.bar:SetPoint("TOPLEFT", ColdGauge.label, "BOTTOMLEFT", 0, -3)
ColdGauge.bar:SetPoint("TOPRIGHT", ColdGauge.value, "BOTTOMRIGHT", 0, -3)

local Status = Frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
Status:SetPoint("TOPLEFT", ColdGauge.bar, "BOTTOMLEFT", 0, -10)
Status:SetPoint("TOPRIGHT", ColdGauge.bar, "BOTTOMRIGHT", 0, -10)
Status:SetHeight(30)
Status:SetJustifyH("LEFT")
Status:SetJustifyV("TOP")

--------------------------------------------------------------------------------
-- Display update
--------------------------------------------------------------------------------

local function UpdateGauge(gauge, value, band)
    local fillR, fillG, fillB = BarFillColor(gauge.key, band)
    gauge.bar:SetStatusBarColor(fillR, fillG, fillB)

    if State.loaded then
        local current = Clamp(value)
        gauge.bar:SetValue(current)
        gauge.value:SetText(string.format("%.0f %%", current))
        gauge.label:SetText(string.format("%s (%s)", gauge.name, BAND_LABELS[gauge.key][band]))

        local r, g, b = BandColor(band)
        gauge.label:SetTextColor(r, g, b)
        gauge.value:SetTextColor(r, g, b)
    else
        gauge.bar:SetValue(0)
        gauge.value:SetText("--")
        gauge.value:SetTextColor(0.65, 0.65, 0.65)
        gauge.label:SetText(gauge.name)
        gauge.label:SetTextColor(0.65, 0.65, 0.65)
    end
end

local function UpdateStatus()
    if not State.loaded then
        if State.persistence == false then
            Status:SetText("|cffff9d00Persistence unavailable|r")
        else
            Status:SetText("|cffffd200Loading...|r")
        end
        return
    end

    local modeText = MODE_LABELS[State.mode] or MODE_LABELS[0]
    if State.mode == 0 then
        modeText = "|cff9d9d9d" .. modeText .. "|r"
    end

    local text = "Mode: " .. modeText
    if State.lethal then
        text = text .. " · |cffff4040Lethal|r"
    end
    if State.hot then
        text = text .. " · |cffff6060Hot|r"
    end
    if not State.persistence then
        text = text .. " · |cffff9d00Not persisted|r"
    end
    Status:SetText(text)
end

local UpdateIcons

local function Update()
    UpdateGauge(SatietyGauge, State.satiety, State.satietyBand)
    UpdateGauge(HydrationGauge, State.hydration, State.hydrationBand)
    UpdateGauge(ColdGauge, State.cold, State.coldBand)
    UpdateStatus()
    UpdateIcons()
end

--------------------------------------------------------------------------------
-- Position (SavedVariables)
--------------------------------------------------------------------------------

local function SavePosition()
    local point, _, relativePoint, x, y = Frame:GetPoint(1)
    if type(point) ~= "string" or type(relativePoint) ~= "string" then
        return
    end

    HungerThirstDB.point = point
    HungerThirstDB.relativePoint = relativePoint
    HungerThirstDB.x = x
    HungerThirstDB.y = y
end

local function ApplySavedPosition()
    Frame:ClearAllPoints()

    local point = HungerThirstDB.point
    local relativePoint = HungerThirstDB.relativePoint
    if type(point) == "string" and type(relativePoint) == "string" then
        Frame:SetPoint(point, UIParent, relativePoint, tonumber(HungerThirstDB.x) or 0, tonumber(HungerThirstDB.y) or 0)
    else
        Frame:SetPoint(DEFAULT_POSITION.point, UIParent, DEFAULT_POSITION.relativePoint, DEFAULT_POSITION.x,
            DEFAULT_POSITION.y)
    end
end

local function ResetPosition()
    HungerThirstDB.point = nil
    HungerThirstDB.relativePoint = nil
    HungerThirstDB.x = nil
    HungerThirstDB.y = nil
    ApplySavedPosition()
end

Frame:SetScript("OnDragStart", function(self)
    self:StartMoving()
end)

Frame:SetScript("OnDragStop", function(self)
    self:StopMovingOrSizing()
    SavePosition()
end)

Frame:SetScript("OnShow", function()
    Update()
end)

--------------------------------------------------------------------------------
-- Status icon bar (permanent)
--
-- Three icons stay visible even when the main frame is hidden.
-- Choice: the bar is movable (drag an icon); its position is saved and
-- "/hungerthirst reset" also brings it back to the bottom right.
--------------------------------------------------------------------------------

local ICON_SIZE = 28
local ICON_SPACING = 4
local ICON_DRAG_THRESHOLD = 4
local ICON_DEFAULT_POSITION = { point = "BOTTOMRIGHT", relativePoint = "BOTTOMRIGHT", x = -24, y = 140 }

local ICON_TEXTURES = {
    satiety   = "Interface\\Icons\\INV_Misc_Food_15",
    hydration = "Interface\\Icons\\INV_Drink_05",
    cold      = "Interface\\Icons\\Spell_Frost_ChillingArmor",
}

local IconsBar = CreateFrame("Frame", nil, UIParent)
IconsBar:SetWidth(ICON_SIZE * 3 + ICON_SPACING * 2)
IconsBar:SetHeight(ICON_SIZE)
IconsBar:SetFrameStrata("MEDIUM")
IconsBar:SetClampedToScreen(true)
IconsBar:SetMovable(true)
IconsBar:EnableMouse(true)
IconsBar:RegisterForDrag("LeftButton")
IconsBar:SetPoint(ICON_DEFAULT_POSITION.point, UIParent, ICON_DEFAULT_POSITION.relativePoint,
    ICON_DEFAULT_POSITION.x, ICON_DEFAULT_POSITION.y)

local function CreateStatusIcon(parent, key, name)
    local icon = CreateFrame("Frame", nil, parent)
    icon:SetWidth(ICON_SIZE)
    icon:SetHeight(ICON_SIZE)
    icon:SetBackdrop({
        bgFile = "Interface\\ChatFrame\\ChatFrameBackground",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true,
        tileSize = 8,
        edgeSize = 8,
        insets = { left = 2, right = 2, top = 2, bottom = 2 },
    })
    icon:SetBackdropColor(0, 0, 0, 0.55)
    icon:SetBackdropBorderColor(0.55, 0.55, 0.55, 0.80)
    icon:EnableMouse(true)
    icon:RegisterForDrag("LeftButton")

    icon.key = key
    icon.tooltipName = name
    icon.texture = icon:CreateTexture(nil, "ARTWORK")
    icon.texture:SetPoint("TOPLEFT", icon, "TOPLEFT", 2, -2)
    icon.texture:SetPoint("BOTTOMRIGHT", icon, "BOTTOMRIGHT", -2, 2)
    icon.texture:SetTexture(ICON_TEXTURES[key])
    icon.texture:SetTexCoord(0.07, 0.93, 0.07, 0.93)
    icon:Show()

    return icon
end

local SatietyIcon = CreateStatusIcon(IconsBar, "satiety", "Satiety")
SatietyIcon:SetPoint("LEFT", IconsBar, "LEFT", 0, 0)

local HydrationIcon = CreateStatusIcon(IconsBar, "hydration", "Hydration")
HydrationIcon:SetPoint("LEFT", SatietyIcon, "RIGHT", ICON_SPACING, 0)

local ColdIcon = CreateStatusIcon(IconsBar, "cold", "Cold")
ColdIcon:SetPoint("LEFT", HydrationIcon, "RIGHT", ICON_SPACING, 0)

local function UpdateIcon(icon)
    local band = State[icon.key .. "Band"] or 0
    local alpha = 0.55
    local borderR, borderG, borderB, borderA = 0.55, 0.55, 0.55, 0.80

    icon.texture:SetDesaturated(false)
    icon.texture:SetVertexColor(1, 1, 1)

    if not State.loaded then
        icon.texture:SetDesaturated(true)
        icon.texture:SetVertexColor(0.55, 0.55, 0.55)
        alpha = 0.40
        borderR, borderG, borderB, borderA = 0.40, 0.40, 0.40, 0.70
    elseif State.mode == 0 then
        alpha = 0.28
        borderR, borderG, borderB, borderA = 0.35, 0.35, 0.35, 0.60
    elseif band ~= 0 then
        alpha = 0.90
        borderR, borderG, borderB = BandColor(band)
        borderA = 1
    end

    icon.texture:SetAlpha(alpha)
    icon:SetBackdropBorderColor(borderR, borderG, borderB, borderA)
end

UpdateIcons = function()
    UpdateIcon(SatietyIcon)
    UpdateIcon(HydrationIcon)
    UpdateIcon(ColdIcon)
end

local function ToggleFrame()
    if Frame:IsShown() then
        Frame:Hide()
    else
        Frame:Show()
        Update()
    end
end

local function IconOnEnter(icon)
    GameTooltip:SetOwner(icon, "ANCHOR_LEFT")
    GameTooltip:AddLine(icon.tooltipName, 1.00, 0.82, 0.25)

    local band = State[icon.key .. "Band"] or 0
    if State.loaded then
        GameTooltip:AddLine(string.format("Value: %d/%d", math.floor(Clamp(State[icon.key]) + 0.5), MAX_VALUE),
            1, 1, 1)
        local r, g, b = BandColor(band)
        GameTooltip:AddLine("State: " .. (BAND_LABELS[icon.key][band] or BAND_LABELS[icon.key][0]), r, g, b)
    else
        GameTooltip:AddLine("Value: --/" .. MAX_VALUE, 0.65, 0.65, 0.65)
        GameTooltip:AddLine("State: loading...", 0.65, 0.65, 0.65)
    end

    local modeText = MODE_LABELS[State.mode] or MODE_LABELS[0]
    if State.mode == 0 then
        GameTooltip:AddLine("Mode: " .. modeText, 0.65, 0.65, 0.65)
    else
        GameTooltip:AddLine("Mode: " .. modeText, 1, 1, 1)
    end

    GameTooltip:AddLine("Click to show/hide the bars", 1, 1, 1, true)
    GameTooltip:Show()
end

local function IconOnLeave()
    GameTooltip:Hide()
end

local function SaveIconPosition()
    local point, _, relativePoint, x, y = IconsBar:GetPoint(1)
    if type(point) ~= "string" or type(relativePoint) ~= "string" then
        return
    end

    HungerThirstDB.iconPoint = point
    HungerThirstDB.iconRelativePoint = relativePoint
    HungerThirstDB.iconX = x
    HungerThirstDB.iconY = y
end

local function ApplySavedIconPosition()
    IconsBar:ClearAllPoints()

    local point = HungerThirstDB.iconPoint
    local relativePoint = HungerThirstDB.iconRelativePoint
    if type(point) == "string" and type(relativePoint) == "string" then
        IconsBar:SetPoint(point, UIParent, relativePoint, tonumber(HungerThirstDB.iconX) or 0,
            tonumber(HungerThirstDB.iconY) or 0)
    else
        IconsBar:SetPoint(ICON_DEFAULT_POSITION.point, UIParent, ICON_DEFAULT_POSITION.relativePoint,
            ICON_DEFAULT_POSITION.x, ICON_DEFAULT_POSITION.y)
    end
end

local function ResetIconPosition()
    HungerThirstDB.iconPoint = nil
    HungerThirstDB.iconRelativePoint = nil
    HungerThirstDB.iconX = nil
    HungerThirstDB.iconY = nil
    ApplySavedIconPosition()
end

local function ApplyIconsShown()
    if HungerThirstDB.iconsShown == false then
        GameTooltip:Hide()
        IconsBar:Hide()
    else
        IconsBar:Show()
        UpdateIcons()
    end
end

local IconPress = { x = nil, y = nil, consumed = false }

local function CursorMovedFromPress()
    if type(IconPress.x) ~= "number" or type(IconPress.y) ~= "number" then
        return true
    end

    local x, y = GetCursorPosition()
    local scale = IconsBar:GetEffectiveScale()
    if not scale or scale == 0 then
        scale = 1
    end

    local dx = (x - IconPress.x) / scale
    local dy = (y - IconPress.y) / scale
    return (dx * dx + dy * dy) > (ICON_DRAG_THRESHOLD * ICON_DRAG_THRESHOLD)
end

local function HandleIconClick()
    if IconPress.consumed then
        return
    end
    IconPress.consumed = true

    local moved = CursorMovedFromPress()
    IconPress.x, IconPress.y = nil, nil

    if not moved then
        ToggleFrame()
    end
end

local function OnIconMouseDown(_, button)
    if button ~= "LeftButton" then
        return
    end
    IconPress.x, IconPress.y = GetCursorPosition()
    IconPress.consumed = false
end

local function OnIconMouseUp(_, button)
    if button ~= "LeftButton" then
        return
    end
    HandleIconClick()
end

local function OnIconDragStart()
    GameTooltip:Hide()
    IconsBar:StartMoving()
end

local function OnIconDragStop()
    IconsBar:StopMovingOrSizing()
    SaveIconPosition()
    HandleIconClick()
end

for _, icon in ipairs({ SatietyIcon, HydrationIcon, ColdIcon }) do
    icon:SetScript("OnMouseDown", OnIconMouseDown)
    icon:SetScript("OnMouseUp", OnIconMouseUp)
    icon:SetScript("OnDragStart", OnIconDragStart)
    icon:SetScript("OnDragStop", OnIconDragStop)
    icon:SetScript("OnEnter", function(self)
        IconOnEnter(self)
    end)
    icon:SetScript("OnLeave", IconOnLeave)
end

IconsBar:SetScript("OnMouseDown", OnIconMouseDown)
IconsBar:SetScript("OnMouseUp", OnIconMouseUp)
IconsBar:SetScript("OnDragStart", OnIconDragStart)
IconsBar:SetScript("OnDragStop", OnIconDragStop)

ApplyIconsShown()

--------------------------------------------------------------------------------
-- Protocol
--------------------------------------------------------------------------------

local function ParsePayload(text)
    if type(text) ~= "string" then
        return nil
    end

    local fields = { strsplit("\t", text) }
    if fields[1] == ADDON_PREFIX then
        table.remove(fields, 1)
    end

    if #fields < 8 then
        return nil
    end

    local satiety = tonumber(fields[1])
    local hydration = tonumber(fields[2])
    if not satiety or not hydration then
        return nil
    end

    -- Fields 9-11 are appended to the original eight-field protocol, so they
    -- are optional: an old server leaves cold at 0 and hot at false.
    local cold = tonumber(fields[9]) or 0
    local coldBand = NormalizeBand(fields[10])
    local hot = (tonumber(fields[11]) or 0) ~= 0

    return {
        satiety = Clamp(satiety),
        hydration = Clamp(hydration),
        mode = NormalizeMode(fields[3]),
        lethal = (tonumber(fields[4]) or 0) ~= 0,
        loaded = (tonumber(fields[5]) or 0) ~= 0,
        persistence = (tonumber(fields[6]) or 0) ~= 0,
        satietyBand = NormalizeBand(fields[7]),
        hydrationBand = NormalizeBand(fields[8]),
        cold = Clamp(cold),
        coldBand = coldBand,
        hot = hot,
    }
end

local function ApplyPayload(payload)
    State.satiety = payload.satiety
    State.hydration = payload.hydration
    State.mode = payload.mode
    State.lethal = payload.lethal
    State.loaded = payload.loaded
    State.persistence = payload.persistence
    State.satietyBand = payload.satietyBand
    State.hydrationBand = payload.hydrationBand
    State.cold = payload.cold
    State.coldBand = payload.coldBand
    State.hot = payload.hot
    Update()
end

local function OnAddonMessage(prefix, text, channel, sender)
    if prefix ~= ADDON_PREFIX then
        return
    end
    -- The server sends an addon whisper from the player to the player.
    if channel ~= "WHISPER" or sender ~= UnitName("player") then
        return
    end

    local payload = ParsePayload(text)
    if payload then
        ApplyPayload(payload)
    end
end

--------------------------------------------------------------------------------
-- Events
--------------------------------------------------------------------------------

local EventFrame = CreateFrame("Frame")
EventFrame:RegisterEvent("CHAT_MSG_ADDON")
EventFrame:RegisterEvent("PLAYER_LOGIN")
EventFrame:SetScript("OnEvent", function(_, event, ...)
    if event == "CHAT_MSG_ADDON" then
        local prefix, text, channel, sender = ...
        OnAddonMessage(prefix, text, channel, sender)
    elseif event == "PLAYER_LOGIN" then
        if RegisterAddonMessagePrefix then
            RegisterAddonMessagePrefix(ADDON_PREFIX)
        end
        ApplySavedPosition()
        ApplySavedIconPosition()
        ApplyIconsShown()
        Update()
    end
end)

if RegisterAddonMessagePrefix then
    RegisterAddonMessagePrefix(ADDON_PREFIX)
end

--------------------------------------------------------------------------------
-- User command
--------------------------------------------------------------------------------

-- "/hungerthirst" is a purely client-side command: the equivalent server
-- command is written ".hungerthirst" and therefore does not conflict.
SLASH_HUNGERTHIRST1 = "/hungerthirst"
SLASH_HUNGERTHIRST2 = "/ht"
SlashCmdList["HUNGERTHIRST"] = function(message)
    local command = string.lower(message or "")
    command = string.match(command, "^%s*(.-)%s*$") or ""

    if command == "reset" then
        ResetPosition()
        ResetIconPosition()
        Frame:Show()
        Update()
    elseif command == "icons" then
        HungerThirstDB.iconsShown = (HungerThirstDB.iconsShown == false)
        ApplyIconsShown()
    else
        ToggleFrame()
    end
end

Update()

--------------------------------------------------------------------------------
-- Aura tooltips
--
-- The server reuses four stock Blizzard spells and patches their server-side
-- data, so the client tooltips keep the original, unrelated descriptions.
-- These overrides replace them with the module's effects and remedies.
--------------------------------------------------------------------------------

local AURA_TOOLTIPS = {
    [52749] = {
        title = "Faim — Appétit vorace",
        lines = {
            "Vitesse de déplacement : -10 % par défaut",
            "Dégâts infligés : -10 % par défaut",
            "Dégâts subis : +10 % par défaut",
            "Pourcentages réels : voir la ligne de chat à l'entrée de l'état.",
            "Remède : mange pour repasser au-dessus de 32 % de satiété (30 % + 2, valeurs par défaut). " ..
                "Depuis 0 %, une bouchée remonte à 30 % et laisse l'état affamé, mais reste dans " ..
                "l'état affaibli ; une seconde bouchée sort des deux.",
        },
    },
    [55316] = {
        title = "Faim — Faim dévorante",
        lines = {
            "Vitesse de déplacement : -30 % par défaut",
            "Dégâts infligés : -30 % par défaut",
            "Dégâts subis : +30 % par défaut",
            "Pourcentages réels : voir la ligne de chat à l'entrée de l'état.",
            "Remède : mange pour repasser au-dessus de 32 % de satiété (30 % + 2, valeurs par défaut). " ..
                "Depuis 0 %, une bouchée remonte à 30 % et laisse l'état affamé, mais reste dans " ..
                "l'état affaibli ; une seconde bouchée sort des deux.",
        },
    },
    [25696] = {
        title = "Soif — Très soif",
        lines = {
            "Régénération de mana (Esprit) : -10 % par défaut",
            "Dégâts magiques infligés : -10 % par défaut",
            "Soins prodigués : -10 % par défaut",
            "Pourcentages réels : voir la ligne de chat à l'entrée de l'état.",
            "Remède : bois pour repasser au-dessus de 32 % d'hydratation (30 % + 2, valeurs par défaut).",
        },
    },
    [5909] = {
        title = "Soif — Déshydraté",
        lines = {
            "Régénération de mana (Esprit) : -30 % par défaut",
            "Dégâts magiques infligés : -30 % par défaut",
            "Soins prodigués : -30 % par défaut",
            "Pourcentages réels : voir la ligne de chat à l'entrée de l'état.",
            "Remède : bois ; une boisson rend 35 points par défaut et sort de l'état.",
        },
    },
}

-- 14870 ("Drunken Stupor") is the retired dehydrated spell, replaced by 5909
-- ("Watered-down Beer"); its stock client effect is a stun and the module strips it.
AURA_TOOLTIPS[14870] = AURA_TOOLTIPS[5909]

-- UnitAura returns the spell ID as its 11th value; this fallback covers
-- clients that omit it and matches the English aura names.
local AURA_TOOLTIPS_BY_NAME = {
    ["Voracious Appetite"] = AURA_TOOLTIPS[52749],
    ["Ravenous Hunger"] = AURA_TOOLTIPS[55316],
    ["Cosmetic - Drink"] = AURA_TOOLTIPS[25696],
    ["Drink"] = AURA_TOOLTIPS[25696],
    ["Boisson"] = AURA_TOOLTIPS[25696],
    ["Drunken Stupor"] = AURA_TOOLTIPS[5909],
    ["Watered-down Beer"] = AURA_TOOLTIPS[5909],
}

if GameTooltip and GameTooltip.SetUnitAura then
    hooksecurefunc(GameTooltip, "SetUnitAura", function(self, unit, index, filter)
        if unit ~= "player" then
            return
        end

        local name, _, _, _, _, _, _, _, _, _, spellId = UnitAura(unit, index, filter)
        local entry
        if spellId then
            entry = AURA_TOOLTIPS[spellId]
        elseif name then
            entry = AURA_TOOLTIPS_BY_NAME[name]
        end
        if not entry then
            return
        end

        self:ClearLines()
        self:AddLine(entry.title, 1, 0.82, 0.25)
        for _, line in ipairs(entry.lines) do
            self:AddLine(line, 1, 1, 1, true)
        end
        self:Show()
    end)
end
