#pragma once
#include <string>

#include "analysis.h"
#include "gc/dol.h"

namespace recomp {

struct CodegenStats {
    size_t instructions = 0;
    size_t unimplemented = 0;
    std::map<std::string, size_t> unimplemented_by_op;
};

struct CodegenOptions {
    std::string out_dir;
    size_t functions_per_file = 256;
    bool emit_comments = true;
};

bool generate(const gc::Dol& dol, const Analysis& an, const CodegenOptions& opt, CodegenStats& stats,
              std::string& error);

} // namespace recomp
