return function(env)
    local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local reverse = {}
    for i = 1, #alphabet do reverse[string.byte(alphabet, i)] = i - 1 end
    env.base64encode = function(data)
        assert(type(data) == "string", "base64encode expects a string")
        local out = table.create(math.ceil(#data / 3))
        for i = 1, #data, 3 do
            local a, b, c = string.byte(data, i, i + 2)
            local n = a * 65536 + (b or 0) * 256 + (c or 0)
            local x, y = math.floor(n / 262144) % 64 + 1, math.floor(n / 4096) % 64 + 1
            local z, w = math.floor(n / 64) % 64 + 1, n % 64 + 1
            out[#out + 1] = string.sub(alphabet, x, x) .. string.sub(alphabet, y, y)
                .. (b and string.sub(alphabet, z, z) or "=") .. (c and string.sub(alphabet, w, w) or "=")
        end
        return table.concat(out)
    end
    env.base64decode = function(data)
        assert(type(data) == "string", "base64decode expects a string")
        data = string.gsub(data, "%s", "")
        assert(#data % 4 == 0, "invalid base64 length")
        local out = table.create(#data / 4)
        for i = 1, #data, 4 do
            local a, b, c, d = string.byte(data, i, i + 3)
            local x, y, z, w = reverse[a], reverse[b], reverse[c], reverse[d]
            assert(x and y and (z or c == 61) and (w or d == 61), "invalid base64 character")
            assert((c ~= 61 or d == 61) and (c ~= 61 and d ~= 61 or i + 3 == #data), "invalid base64 padding")
            assert(c ~= 61 or y % 16 == 0, "invalid base64 padding bits")
            assert(d ~= 61 or c == 61 or z % 4 == 0, "invalid base64 padding bits")
            local n = x * 262144 + y * 4096 + (z or 0) * 64 + (w or 0)
            out[#out + 1] = string.char(math.floor(n / 65536))
                .. (c ~= 61 and string.char(math.floor(n / 256) % 256) or "")
                .. (d ~= 61 and string.char(n % 256) or "")
        end
        return table.concat(out)
    end
    local function lengthBytes(length)
        local out = {}
        while length >= 255 do out[#out + 1] = string.char(255); length -= 255 end
        out[#out + 1] = string.char(length)
        return table.concat(out)
    end
    env.lz4compress = function(data)
        assert(type(data) == "string", "lz4compress expects a string")
        local size = #data
        assert(size <= 8 * 1024 * 1024, "input exceeds 8 MiB")
        local input = buffer.fromstring(data)
        local dictionary, chunks = {}, {}
        local anchor, position = 0, 0
        while position <= size - 12 do
            local key = buffer.readu32(input, position)
            local previous = dictionary[key]
            dictionary[key] = position
            if previous == nil or position - previous > 65535 then position += 1; continue end
            local length = 4
            while position + length < size - 5 and buffer.readu8(input, previous + length) == buffer.readu8(input, position + length) do length += 1 end
            local literals = position - anchor
            chunks[#chunks + 1] = string.char(math.min(literals, 15) * 16 + math.min(length - 4, 15))
            if literals >= 15 then chunks[#chunks + 1] = lengthBytes(literals - 15) end
            chunks[#chunks + 1] = string.sub(data, anchor + 1, position)
            local distance = position - previous
            chunks[#chunks + 1] = string.char(distance % 256, math.floor(distance / 256))
            if length >= 19 then chunks[#chunks + 1] = lengthBytes(length - 19) end
            position += length
            anchor = position
        end
        local literals = size - anchor
        chunks[#chunks + 1] = string.char(math.min(literals, 15) * 16)
        if literals >= 15 then chunks[#chunks + 1] = lengthBytes(literals - 15) end
        chunks[#chunks + 1] = string.sub(data, anchor + 1)
        return table.concat(chunks)
    end
    env.lz4decompress = function(data)
        assert(type(data) == "string" and #data > 0, "invalid LZ4 block")
        local input = buffer.fromstring(data)
        local offset, expected = 0, nil
        if #data >= 12 and buffer.readu32(input, 8) == 0 then
            local compressed, size = buffer.readu32(input, 0), buffer.readu32(input, 4)
            if compressed == 0 and size == #data - 12 then return string.sub(data, 13) end
            if compressed == #data - 12 then offset, expected = 12, size end
        end
        local function decode(output)
            local read, write = offset, 0
            local function extend(length)
                if length == 15 then
                    while true do
                        assert(read < #data, "truncated LZ4 length")
                        local byte = buffer.readu8(input, read)
                        read += 1; length += byte
                        assert(length <= 8 * 1024 * 1024, "output exceeds 8 MiB")
                        if byte ~= 255 then break end
                    end
                end
                return length
            end
            while read < #data do
                local token = buffer.readu8(input, read)
                read += 1
                local literals = extend(math.floor(token / 16))
                assert(read + literals <= #data and write + literals <= 8 * 1024 * 1024, "invalid LZ4 literal")
                if output then buffer.copy(output, write, input, read, literals) end
                read += literals; write += literals
                if read == #data then break end
                assert(read + 2 <= #data, "truncated LZ4 offset")
                local distance = buffer.readu16(input, read)
                read += 2
                assert(distance > 0 and distance <= write, "invalid LZ4 offset")
                local length = extend(token % 16) + 4
                assert(write + length <= 8 * 1024 * 1024, "LZ4 output overflow")
                if output then
                    for i = 0, length - 1 do buffer.writeu8(output, write + i, buffer.readu8(output, write + i - distance)) end
                end
                write += length
            end
            return write
        end
        local size=decode(nil)
        assert(not expected or size==expected,"LZ4 output length mismatch")
        local output=buffer.create(size)
        decode(output)
        return buffer.tostring(output)
    end
end
