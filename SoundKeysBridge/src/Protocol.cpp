#include "Protocol.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace skb {

static std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

Fields parseFields(const std::string& text) {
    if (text.size() > 65536 || text.find('\0') != std::string::npos) {
        throw Error("bad_protocol", "Input exceeds 64 KiB or contains a NUL");
    }

    Fields fields;
    std::istringstream input(text);
    std::string line;
    unsigned lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) {
            line.erase(0, 3);
        }

        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            throw Error("bad_protocol", "Missing '=' at line " + std::to_string(lineNumber));
        }

        const auto key = trim(line.substr(0, separator));
        const auto value = trim(line.substr(separator + 1));
        if (key.empty()
            || key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            throw Error("bad_protocol", "Invalid key at line " + std::to_string(lineNumber));
        }
        if (!fields.emplace(key, value).second) {
            throw Error("bad_protocol", "Duplicate key: " + key);
        }
    }

    return fields;
}

std::string require(const Fields& fields, const std::string& key) {
    const auto found = fields.find(key);
    if (found == fields.end() || found->second.empty()) {
        throw Error("bad_protocol", "Missing field: " + key);
    }
    return found->second;
}

std::string optional(const Fields& fields, const std::string& key, const std::string& fallback) {
    const auto found = fields.find(key);
    return found == fields.end() ? fallback : found->second;
}

int integer(const Fields& fields, const std::string& key, int low, int high) {
    const auto text = require(fields, key);
    if (text.find_first_not_of("0123456789-") != std::string::npos) {
        throw Error("bad_protocol", "Not an integer: " + key);
    }

    try {
        size_t used = 0;
        const auto value = std::stoll(text, &used);
        if (used != text.size() || value < low || value > high) {
            throw std::out_of_range(key);
        }
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw Error("bad_protocol", "Integer outside allowed range: " + key);
    }
}

static double number(const Fields& fields, const std::string& key, double low, double high) {
    std::istringstream text(require(fields, key));
    text.imbue(std::locale::classic());

    double value = 0;
    text >> std::noskipws >> value;
    if (!text || !text.eof() || !std::isfinite(value) || value < low || value > high) {
        throw Error("bad_protocol", "Invalid number: " + key);
    }
    return value;
}

static void known(const Fields& fields, const std::set<std::string>& keys) {
    for (const auto& field : fields) {
        if (!keys.count(field.first)) {
            throw Error("bad_protocol", "Unknown field: " + field.first);
        }
    }
}

bool validJobId(const std::string& jobId) {
    return !jobId.empty()
        && jobId.size() <= 64
        && jobId.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")
            == std::string::npos;
}

Job parseJob(const std::string& text) {
    const auto fields = parseFields(text);
    known(fields, {
        "protocol",
        "job_id",
        "input_project",
        "output_project",
        "profile",
        "comp_id",
        "layer_index",
        "layer_name",
        "effect_index",
    });
    if (require(fields, "protocol") != "1") {
        throw Error("bad_protocol", "Only protocol=1 is supported");
    }

    Job job;
    job.id = require(fields, "job_id");
    if (!validJobId(job.id)) {
        throw Error("bad_protocol", "Invalid job_id");
    }

    job.input = require(fields, "input_project");
    job.output = require(fields, "output_project");
    job.profile = require(fields, "profile");
    job.layerName = require(fields, "layer_name");
    job.compId = integer(fields, "comp_id", 1, 2147483647);
    job.layerIndex = integer(fields, "layer_index", 1, 1000000);
    job.effectIndex = integer(fields, "effect_index", 1, 1000000);
    return job;
}

Profile parseProfile(const std::string& text) {
    const auto fields = parseFields(text);
    known(fields, {
        "protocol",
        "profile_id",
        "effect_match",
        "command_match",
        "command_name",
        "command_index",
        "mode",
        "contextless",
        "release_drag",
        "frame_left",
        "frame_top",
        "frame_width",
        "frame_height",
        "click_x",
        "click_y",
        "outputs",
        "min_keys",
        "max_keys",
        "start_tolerance_seconds",
        "end_tolerance_seconds",
        "max_gap_seconds",
    });
    if (require(fields, "protocol") != "1") {
        throw Error("bad_profile", "Only protocol=1 is supported");
    }

    Profile profile;
    profile.id = require(fields, "profile_id");
    profile.effectMatch = require(fields, "effect_match");
    profile.commandMatch = optional(fields, "command_match", "");
    profile.commandName = require(fields, "command_name");
    profile.commandIndex = fields.count("command_index")
        ? integer(fields, "command_index", -1, 100000)
        : -1;
    profile.mode = require(fields, "mode");
    if (profile.mode != "standard_button" && profile.mode != "custom_event") {
        throw Error("bad_profile", "mode must be standard_button or custom_event");
    }

    if (profile.mode == "custom_event") {
        profile.contextless = integer(fields, "contextless", 0, 1) == 1;
        if (!profile.contextless) {
            throw Error(
                "no_event_context",
                "contextless=1 is required for custom event dispatch");
        }

        profile.releaseDrag = integer(fields, "release_drag", 0, 1) == 1;
        profile.left = integer(fields, "frame_left", -16000, 16000);
        profile.top = integer(fields, "frame_top", -16000, 16000);
        profile.width = integer(fields, "frame_width", 1, 16000);
        profile.height = integer(fields, "frame_height", 1, 16000);
        profile.clickX = integer(fields, "click_x", 0, profile.width - 1);
        profile.clickY = integer(fields, "click_y", 0, profile.height - 1);
    }

    std::istringstream outputList(require(fields, "outputs"));
    std::string part;
    while (std::getline(outputList, part, ',')) {
        const int value = integer({{"n", trim(part)}}, "n", 1, 3);
        if (std::find(profile.outputs.begin(), profile.outputs.end(), value) != profile.outputs.end()) {
            throw Error("bad_profile", "Duplicate output");
        }
        profile.outputs.push_back(value);
    }
    if (profile.outputs.empty() || require(fields, "outputs").back() == ',') {
        throw Error("bad_profile", "Empty output selection");
    }

    profile.minKeys = integer(fields, "min_keys", 1, 1000000);
    profile.maxKeys = integer(fields, "max_keys", profile.minKeys, 1000000);
    profile.startTolerance = number(fields, "start_tolerance_seconds", 0, 10);
    profile.endTolerance = number(fields, "end_tolerance_seconds", 0, 10);
    profile.maxGap = number(fields, "max_gap_seconds", 0.000001, 10);
    return profile;
}

std::string jsonString(const std::string& value) {
    std::ostringstream out;
    out << '"';

    for (unsigned char character : value) {
        switch (character) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (character < 32) {
                out << "\\u"
                    << std::hex
                    << std::setw(4)
                    << std::setfill('0')
                    << static_cast<unsigned>(character)
                    << std::dec;
            } else {
                out << static_cast<char>(character);
            }
            break;
        }
    }

    out << '"';
    return out.str();
}

void verifyOutputs(
    const Profile& profile,
    const std::vector<Output>& outputs,
    double start,
    double duration) {
    if (!std::isfinite(start) || !std::isfinite(duration) || duration <= 0) {
        throw Error("bad_work_area", "Invalid work area");
    }
    if (outputs.size() != profile.outputs.size()) {
        throw Error("verification_failed", "Wrong output set");
    }

    for (size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
        const auto& output = outputs[outputIndex];
        if (output.number != profile.outputs[outputIndex]
            || output.keys.size() < static_cast<size_t>(profile.minKeys)
            || output.keys.size() > static_cast<size_t>(profile.maxKeys)) {
            throw Error(
                "verification_failed",
                "Missing, excessive, or insufficient keys on Output "
                    + std::to_string(output.number));
        }

        const double end = start + duration;
        if (std::abs(output.keys.front().time - start) > profile.startTolerance + 1e-7
            || output.keys.back().time < end - profile.endTolerance - 1e-7
            || output.keys.back().time > end + 1e-7) {
            throw Error("verification_failed", "Keyframes do not cover the requested work area");
        }

        for (size_t keyIndex = 0; keyIndex < output.keys.size(); ++keyIndex) {
            const auto& key = output.keys[keyIndex];
            if (!std::isfinite(key.time)
                || !std::isfinite(key.value)
                || key.time < start - profile.startTolerance - 1e-7
                || key.time > end + 1e-7) {
                throw Error("verification_failed", "Nonfinite or out-of-range key");
            }
            if (keyIndex
                && (key.time <= output.keys[keyIndex - 1].time
                    || key.time - output.keys[keyIndex - 1].time > profile.maxGap + 1e-7)) {
                throw Error("verification_failed", "Duplicate/unordered key times or excessive gap");
            }
        }
    }
}

std::string outputsJson(const std::vector<Output>& outputs) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << '[';

    for (size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex) {
        if (outputIndex) {
            out << ',';
        }

        out << "{\"output\":" << outputs[outputIndex].number << ",\"keys\":[";
        for (size_t keyIndex = 0; keyIndex < outputs[outputIndex].keys.size(); ++keyIndex) {
            if (keyIndex) {
                out << ',';
            }

            const auto& key = outputs[outputIndex].keys[keyIndex];
            if (!std::isfinite(key.time) || !std::isfinite(key.value)) {
                throw Error("verification_failed", "Cannot serialize nonfinite key");
            }
            out << '[' << key.time << ',' << key.value << ']';
        }
        out << "]}";
    }

    out << ']';
    return out.str();
}

}  // namespace skb
