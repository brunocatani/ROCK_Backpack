#pragma once
#include <RobCoPALM/Icons.h>
namespace rock_backpack::ui {
using Icon = palm::api::Icon;
inline constexpr unsigned kIconSheetCount = 7;
static_assert(static_cast<unsigned>(Icon::Count) == kIconSheetCount * 16);
}
