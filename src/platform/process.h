#pragma once

#include <functional>
#include <string>
#include <vector>

// Launch without a shell; drain stdout even if the consumer reports an error.
bool run_process(const std::vector<std::string>& arguments,
                 const std::function<bool(const char*, size_t)>& on_data);
