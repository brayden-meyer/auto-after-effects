#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace skb {

struct Error : std::runtime_error {
    std::string code;

    Error(const std::string& errorCode, const std::string& message)
        : std::runtime_error(message),
          code(errorCode) {}
};

using Fields = std::map<std::string, std::string>;

Fields parseFields(const std::string& text);
std::string jsonString(const std::string& value);
std::string require(const Fields& fields, const std::string& key);
std::string optional(const Fields& fields, const std::string& key, const std::string& fallback);
int integer(const Fields& fields, const std::string& key, int low, int high);
bool validJobId(const std::string& jobId);

struct Job {
    std::string id;
    std::string input;
    std::string output;
    std::string profile;
    std::string layerName;
    int compId;
    int layerIndex;
    int effectIndex;
};

Job parseJob(const std::string& text);

struct Profile {
    std::string id;
    std::string effectMatch;
    std::string commandMatch;
    std::string commandName;
    std::string mode;

    // Native index is optional, never inferred from a match-name suffix.
    int commandIndex = -1;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    int clickX = 0;
    int clickY = 0;
    bool contextless = false;
    bool releaseDrag = false;

    std::vector<int> outputs;
    double startTolerance;
    double endTolerance;
    double maxGap;
    int minKeys;
    int maxKeys;
};

Profile parseProfile(const std::string& text);

struct Key {
    double time;
    double value;
};

struct Output {
    int number;
    std::vector<Key> keys;
};

// Freshness is separately enforced by requiring zero keys before dispatch.
void verifyOutputs(
    const Profile& profile,
    const std::vector<Output>& outputs,
    double start,
    double duration);

std::string outputsJson(const std::vector<Output>& outputs);

}  // namespace skb
