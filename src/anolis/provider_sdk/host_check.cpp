#include "anolis/provider_sdk/host_check.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

namespace anolis::provider_sdk::host_check {
namespace {

// Length of the valid UTF-8 sequence starting at text[i], or 0 if there is none.
std::size_t utf8_sequence_length(const std::string& text, std::size_t i) {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    unsigned int min = 0;
    unsigned int code = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        min = 0x80;
        code = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        min = 0x800;
        code = lead & 0x0FU;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        min = 0x10000;
        code = lead & 0x07U;
    } else {
        return 0;
    }
    if (i + length > text.size()) {
        return 0;
    }
    for (std::size_t k = 1; k < length; ++k) {
        if ((byte(i + k) & 0xC0U) != 0x80U) {
            return 0;
        }
        code = (code << 6U) | (byte(i + k) & 0x3FU);
    }
    if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

// A JSON string literal. Details and remedies carry paths and strerror text from
// the host, so bytes that are not valid UTF-8 become U+FFFD rather than
// producing a document a JSON parser rejects.
std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            switch (c) {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
            }
            ++i;
            continue;
        }
        const std::size_t length = utf8_sequence_length(text, i);
        if (length == 0) {
            out += "\\ufffd";
            ++i;
        } else {
            out.append(text, i, length);
            i += length;
        }
    }
    out += '"';
    return out;
}

}  // namespace

const char* to_string(Status status) {
    switch (status) {
        case Status::Met:
            return "met";
        case Status::Unmet:
            return "unmet";
        case Status::Unknown:
            return "unknown";
    }
    return "unknown";
}

int exit_code(const std::vector<Requirement>& requirements) {
    const bool unmet = std::any_of(requirements.begin(), requirements.end(),
                                   [](const Requirement& r) { return r.status == Status::Unmet; });
    return unmet ? 1 : 0;
}

std::string envelope(const std::string& provider_name, const std::vector<Requirement>& requirements) {
    std::string out = "{\"check_host_version\":" + std::to_string(kCheckHostVersion);
    if (!provider_name.empty()) {
        out += ",\"provider\":" + json_string(provider_name);
    }
    out += ",\"requirements\":[";
    for (std::size_t i = 0; i < requirements.size(); ++i) {
        const Requirement& r = requirements[i];
        if (r.id.empty()) {
            throw std::invalid_argument("host check requirement has an empty id");
        }
        if (i > 0) {
            out += ',';
        }
        out += "{\"id\":" + json_string(r.id) + ",\"status\":" + json_string(to_string(r.status));
        if (!r.detail.empty()) {
            out += ",\"detail\":" + json_string(r.detail);
        }
        if (!r.remedy.empty()) {
            out += ",\"remedy\":" + json_string(r.remedy);
        }
        out += '}';
    }
    out += "]}";
    return out;
}

int write_envelope(std::ostream& out, const std::string& provider_name, const std::vector<Requirement>& requirements) {
    out << envelope(provider_name, requirements) << '\n';
    return exit_code(requirements);
}

std::map<std::string, std::string> readiness_diagnostics(const std::vector<Requirement>& requirements) {
    std::map<std::string, std::string> diagnostics;
    std::string unmet;
    for (const Requirement& r : requirements) {
        if (r.status != Status::Unmet) {
            continue;
        }
        if (!unmet.empty()) {
            unmet += "; ";
        }
        unmet += r.id;
        if (!r.detail.empty()) {
            unmet += ": " + r.detail;
        }
    }
    if (unmet.empty()) {
        diagnostics["host_check"] = "ok";
    } else {
        diagnostics["host_check"] = "unmet";
        diagnostics["host_unmet"] = unmet;
    }
    return diagnostics;
}

}  // namespace anolis::provider_sdk::host_check
