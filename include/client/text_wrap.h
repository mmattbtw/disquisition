#pragma once

#include <string>
#include <vector>

namespace chat {

// Greedy word wrap for the message pane. The first line starts with `prefix`
// (such as "12:34 matt: ") and later lines get a hanging indent as deep as
// the prefix, so wrapped text lines up under the body instead of under the
// timestamp. A word longer than a whole line is split across lines. Runs of
// whitespace in `body` collapse to single spaces.
std::vector<std::string> wrapText(const std::string& prefix, const std::string& body, int width);

} // namespace chat
