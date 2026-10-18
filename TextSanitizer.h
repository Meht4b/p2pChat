#pragma once

#include <string>

// A username we accept from the local user: 1..32 characters of A-Z a-z 0-9 . _ -
bool isValidUsername(const std::string& name);

// Makes text received from the network safe to show in a terminal:
//  - invalid UTF-8 becomes '?'
//  - control characters (ESC, BEL, C1 controls) and bidi override characters are removed
//  - tabs become spaces
std::string sanitizeText(const std::string& input);
