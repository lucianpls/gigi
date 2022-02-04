local prefix = "/vsicurl/https://parc.s3.us-west-2.amazonaws.com/vortex/rgb/"
local suffix = "_rgb.tif"

local qs = dofile("querystring.lua")

local function dump(o)
    if type(o) == 'table' then
        local s = '{ '
        for k,v in pairs(o) do
                if type(k) ~= 'number' then k = '"'..k..'"' end
                s = s .. '['..k..'] = ' .. dump(v) .. ','
        end
        return s .. '} '
    else
        return tostring(o)
    end
end

-- Should parse the query string and build the string itself
function query_handler( query_string)
    params = qs.parse(query_string)
    return prefix .. params["ID"] .. suffix
end
