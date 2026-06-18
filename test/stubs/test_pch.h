#pragma once

// Force-included precompiled header for the offline `osf-tests` target. Stands in
// for src/pch.h, which pulls the real CommonLibSF (RE/Starfield.h + SFSE). Here we
// substitute the lightweight RE/REX stubs so the engine-independent logic TUs build
// without the game. Mirrors the standard-library surface of the real pch.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "stubs/re_stub.h"
