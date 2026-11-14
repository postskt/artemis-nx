#include "LuaLoader.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace artc {

namespace {

constexpr std::size_t kMemoryLimit = 64u * 1024u * 1024u;
constexpr int kInstructionCount = 20'000'000;
constexpr int kMaxTableDepth = 64;

struct LuaBudget {
    std::size_t used = 0;
    std::size_t limit = kMemoryLimit;
};

void* lua_allocator(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* budget = static_cast<LuaBudget*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        budget->used = budget->used >= osize ? budget->used - osize : 0;
        return nullptr;
    }
    const std::size_t base = ptr ? osize : 0;
    if (budget->used >= base) {
        const std::size_t without = budget->used - base;
        if (nsize > budget->limit || without > budget->limit - nsize) {
            return nullptr;
        }
    } else if (nsize > budget->limit) {
        return nullptr;
    }
    void* result = std::realloc(ptr, nsize);
    if (!result) return nullptr;
    budget->used = budget->used - base + nsize;
    return result;
}

void instruction_hook(lua_State* L, lua_Debug*) {
    luaL_error(L, "AST Lua instruction limit exceeded");
}

std::string lua_error_message(lua_State* L) {
    const char* message = lua_tostring(L, -1);
    return message ? message : "unknown Lua error";
}

bool is_integer_key(lua_State* L, int index) {
    return lua_type(L, index) == LUA_TNUMBER && lua_isinteger(L, index);
}

nlohmann::json value_to_json(lua_State* L, int index, int depth);

nlohmann::json table_to_json(lua_State* L, int index, int depth) {
    if (depth > kMaxTableDepth) {
        throw std::runtime_error("AST table nesting limit exceeded");
    }
    index = lua_absindex(L, index);
    if (!lua_istable(L, index)) {
        throw std::runtime_error("Lua value is not a table during conversion");
    }
    if (!lua_checkstack(L, 4)) {
        throw std::runtime_error("Lua stack exhausted during conversion");
    }
    std::map<lua_Integer, nlohmann::json> numeric;
    std::map<std::string, nlohmann::json> named;

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const int key_type = lua_type(L, -2);
        if (key_type == LUA_TSTRING) {
            const char* key = lua_tostring(L, -2);
            named[key ? key : ""] = value_to_json(L, -1, depth + 1);
        } else if (key_type == LUA_TNUMBER && lua_isinteger(L, -2)) {
            numeric[lua_tointeger(L, -2)] = value_to_json(L, -1, depth + 1);
        }
        lua_pop(L, 1);
    }

    bool contiguous = !numeric.empty();
    lua_Integer max_key = 0;
    if (!numeric.empty()) {
        max_key = numeric.rbegin()->first;
        if (numeric.begin()->first != 1 || max_key != static_cast<lua_Integer>(numeric.size())) {
            contiguous = false;
        }
        if (contiguous) {
            for (lua_Integer i = 1; i <= max_key; ++i) {
                if (numeric.find(i) == numeric.end()) {
                    contiguous = false;
                    break;
                }
            }
        }
    }

    if (named.empty() && contiguous) {
        nlohmann::json array = nlohmann::json::array();
        for (lua_Integer i = 1; i <= max_key; ++i) array.push_back(numeric[i]);
        return array;
    }

    nlohmann::json object = nlohmann::json::object();
    for (const auto& [key, value] : named) object[key] = value;
    if (!numeric.empty()) {
        nlohmann::json array = nlohmann::json::array();
        const lua_Integer limit = std::max<lua_Integer>(max_key, 0);
        for (lua_Integer i = 1; i <= limit; ++i) {
            auto it = numeric.find(i);
            array.push_back(it == numeric.end() ? nlohmann::json() : it->second);
        }
        object["__array"] = std::move(array);
        for (const auto& [key, value] : numeric) {
            object[std::to_string(key)] = value;
        }
    }
    return object;
}

nlohmann::json value_to_json(lua_State* L, int index, int depth) {
    switch (lua_type(L, index)) {
        case LUA_TNIL:
            return nullptr;
        case LUA_TBOOLEAN:
            return lua_toboolean(L, index) != 0;
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) {
                return static_cast<long long>(lua_tointeger(L, index));
            }
            return lua_tonumber(L, index);
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            return std::string(text ? text : "", length);
        }
        case LUA_TTABLE:
            return table_to_json(L, index, depth);
        default:
            return nullptr;
    }
}

nlohmann::json value_at(const nlohmann::json& value, const char* key) {
    if (value.is_object() && value.contains(key)) return value.at(key);
    return nlohmann::json();
}

std::vector<nlohmann::json> array_part(const nlohmann::json& value) {
    if (value.is_array()) return value.get<std::vector<nlohmann::json>>();
    if (value.is_object() && value.contains("__array") && value["__array"].is_array()) {
        return value["__array"].get<std::vector<nlohmann::json>>();
    }
    return {};
}

std::string string_field(const nlohmann::json& value, const char* key, const std::string& fallback = {}) {
    if (value.is_object() && value.contains(key) && value[key].is_string()) {
        return value[key].get<std::string>();
    }
    return fallback;
}

int int_field(const nlohmann::json& value, const char* key, int fallback = 0) {
    if (value.is_object() && value.contains(key)) {
        if (value[key].is_number_integer()) return value[key].get<int>();
        if (value[key].is_number()) return static_cast<int>(value[key].get<double>());
    }
    return fallback;
}

Command parse_command(const nlohmann::json& value) {
    Command command;
    if (value.is_array()) {
        if (value.empty() || !value[0].is_string()) {
            throw std::runtime_error("command array has no string name");
        }
        command.name = value[0].get<std::string>();
        for (std::size_t i = 1; i < value.size(); ++i) command.args.push_back(value[i]);
        return command;
    }
    if (!value.is_object()) {
        throw std::runtime_error("command is not an object/array");
    }
    const auto array = array_part(value);
    if (array.empty() || !array[0].is_string()) {
        throw std::runtime_error("command object has no __array name");
    }
    command.name = array[0].get<std::string>();
    for (std::size_t i = 1; i < array.size(); ++i) command.args.push_back(array[i]);
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() == "__array" || it.key().rfind("__", 0) == 0) continue;
        if (it.key().find_first_not_of("0123456789") == std::string::npos) continue;
        command.kwargs[it.key()] = it.value();
    }
    return command;
}

std::string fragment_text(const nlohmann::json& fragment) {
    if (fragment.is_string()) return fragment.get<std::string>();
    if (!fragment.is_object()) return {};
    const auto array = array_part(fragment);
    const std::string tag = array.empty() || !array[0].is_string()
                                ? std::string()
                                : array[0].get<std::string>();
    if (tag == "rt2" || tag == "rt" || tag == "br") return "\n";
    if (tag == "ruby" || tag == "/ruby") return {};
    return {};
}

TextUnit parse_text_unit(const nlohmann::json& value) {
    TextUnit unit;
    if (!value.is_object()) return unit;
    const nlohmann::json ja = value_at(value, "ja");
    if (ja.is_object() || ja.is_array()) {
        const nlohmann::json name = value_at(ja, "name");
        const std::string override_name = string_field(name, "text");
        const std::string base_name = string_field(name, "name");
        if (!override_name.empty()) {
            unit.speaker = override_name;
        } else if (!base_name.empty()) {
            unit.speaker = base_name;
        }

        const auto pages = array_part(ja);
        for (const auto& page : pages) {
            const auto fragments = array_part(page);
            std::string line;
            for (const auto& fragment : fragments) line += fragment_text(fragment);
            if (!line.empty()) {
                if (!unit.text.empty()) unit.text.push_back('\n');
                unit.text += line;
            }
        }
    }

    const nlohmann::json voices = value_at(value, "vo");
    for (const auto& voice_value : array_part(voices)) {
        Voice voice;
        voice.file = string_field(voice_value, "file");
        voice.ch = string_field(voice_value, "ch");
        if (!voice.file.empty() && !voice.ch.empty()) unit.voices.push_back(std::move(voice));
    }

    const nlohmann::json selects = value_at(value, "select");
    for (const auto& option : array_part(selects)) {
        if (option.is_string()) unit.select_options.push_back(option.get<std::string>());
    }
    return unit;
}

AstScript parse_ast(const std::string& path, const nlohmann::json& root) {
    if (!root.is_object() || !root.contains("ast")) {
        throw std::runtime_error("AST file has no top-level 'ast' table");
    }
    const auto& ast = root["ast"];
    if (!ast.is_object() && !ast.is_array()) {
        throw std::runtime_error("AST 'ast' value is not a table");
    }

    AstScript script;
    script.path = path;
    const auto raw_blocks = array_part(ast);
    for (std::size_t i = 0; i < raw_blocks.size(); ++i) {
        const auto& raw_block = raw_blocks[i];
        Block block;
        block.index = static_cast<int>(i) + 1;
        block.line = int_field(raw_block, "line");
        for (const auto& raw_command : array_part(raw_block)) {
            block.commands.push_back(parse_command(raw_command));
        }
        script.blocks.push_back(std::move(block));
    }

    const nlohmann::json text_table = value_at(ast, "text");
    if (text_table.is_array()) {
        for (std::size_t i = 0; i < text_table.size(); ++i) {
            script.texts[static_cast<int>(i) + 1] = parse_text_unit(text_table[i]);
        }
    } else if (text_table.is_object()) {
        for (auto it = text_table.begin(); it != text_table.end(); ++it) {
            if (it.key() == "__array") continue;
            try {
                const int index = std::stoi(it.key());
                script.texts[index] = parse_text_unit(it.value());
            } catch (...) {
                // named text entries are not used by this game
            }
        }
        if (text_table.contains("__array") && text_table["__array"].is_array()) {
            const auto values = text_table["__array"];
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (script.texts.find(static_cast<int>(i) + 1) == script.texts.end()) {
                    script.texts[static_cast<int>(i) + 1] = parse_text_unit(values[i]);
                }
            }
        }
    }

    const nlohmann::json label_table = value_at(ast, "label");
    if (label_table.is_object()) {
        for (auto it = label_table.begin(); it != label_table.end(); ++it) {
            if (!it.value().is_object()) continue;
            LabelTarget target;
            target.block = int_field(it.value(), "block");
            target.command = int_field(it.value(), "label");
            target.valid = target.block > 0;
            script.labels[it.key()] = target;
        }
    }
    return script;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read AST: " + path.string());
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || size > 16 * 1024 * 1024) {
        throw std::runtime_error("AST is too large: " + path.string());
    }
    in.seekg(0, std::ios::beg);
    std::string data(static_cast<std::size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    if (!in) throw std::runtime_error("cannot read AST: " + path.string());
    return data;
}

}  // namespace

AstScript LuaLoader::load_ast(const std::filesystem::path& path) const {
    const std::string source = read_file(path);
    LuaBudget budget;
    lua_State* L = lua_newstate(lua_allocator, &budget);
    if (!L) throw std::runtime_error("cannot create Lua state");
    try {
        luaL_requiref(L, "_G", luaopen_base, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(L, 1);
        luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(L, 1);

        for (const char* name : {"dofile", "loadfile", "load", "require",
                                 "collectgarbage", "print", "io", "os",
                                 "package", "debug"}) {
            lua_pushnil(L);
            lua_setglobal(L, name);
        }
        lua_sethook(L, instruction_hook, LUA_MASKCOUNT, kInstructionCount);

        const int loaded = luaL_loadbufferx(L, source.data(), source.size(),
                                            path.string().c_str(), "t");
        if (loaded != LUA_OK) {
            const std::string error = lua_error_message(L);
            throw std::runtime_error("AST load error: " + error);
        }
        const int status = lua_pcall(L, 0, 0, 0);
        if (status != LUA_OK) {
            const std::string error = lua_error_message(L);
            throw std::runtime_error("AST execute error: " + error);
        }
        lua_getglobal(L, "ast");
        if (!lua_istable(L, -1)) {
            throw std::runtime_error("AST file did not define a table 'ast'");
        }
        nlohmann::json root = nlohmann::json::object();
        root["ast"] = table_to_json(L, -1, 0);
        lua_close(L);
        L = nullptr;
        return parse_ast(path.string(), root);
    } catch (...) {
        if (L) lua_close(L);
        throw;
    }
}

}  // namespace artc

