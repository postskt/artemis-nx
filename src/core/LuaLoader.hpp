#pragma once

#include <filesystem>

#include "AstModel.hpp"

namespace artc {

class LuaLoader {
public:
    // Parse a Lua-table `.ast` data chunk in a restricted sandbox.
    // Throws std::runtime_error on syntax, memory, instruction-limit or
    // structural errors.  No os/io/package/debug/load/dofile APIs are exposed.
    AstScript load_ast(const std::filesystem::path& path) const;
};

}  // namespace artc
