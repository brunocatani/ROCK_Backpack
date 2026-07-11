#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NOMMNOSOUND

#include <algorithm>

using std::min;

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <REL/Relocation.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

using namespace std::literals;
namespace logger = F4SE::log;

#define DLLEXPORT __declspec(dllexport)
