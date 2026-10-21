#include "../include/Token.h"
#include <sstream>

std::string Token::toString() const {
    std::ostringstream oss;
    oss << "[" << line << ":" << col << " " << value << "]";
    return oss.str();
}
