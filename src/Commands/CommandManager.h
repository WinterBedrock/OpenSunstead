#pragma once
#include "Command.h"
#include <vector>
#include <memory>
#include <string>

class CommandManager {
public:
    static CommandManager& GetInstance() {
        static CommandManager instance;
        return instance;
    }

    void Initialize();
    void ExecuteCommand(const std::string& message);
    void PrintToChat(const std::string& message);

    std::string mPrefix = ".";
    std::vector<std::shared_ptr<Command>> mCommands;

private:
    CommandManager() = default;
};
