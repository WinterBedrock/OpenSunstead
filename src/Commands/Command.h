#pragma once
#include <string>
#include <vector>

class Command {
public:
    std::string mName;
    std::string mDescription;
    std::vector<std::string> mAliases;

    Command(const std::string& name, const std::string& description, const std::vector<std::string>& aliases = {})
        : mName(name), mDescription(description), mAliases(aliases) {}

    virtual ~Command() = default;

    virtual void Execute(const std::vector<std::string>& args) = 0;
};
