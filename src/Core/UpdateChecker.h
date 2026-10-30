#pragma once
#include <string>
#include <atomic>
#include <thread>

class UpdateChecker {
public:
    enum class State { Idle, Checking, UpToDate, UpdateAvailable, Failed };

    static UpdateChecker& GetInstance() {
        static UpdateChecker instance;
        return instance;
    }

    void Start();
    void Shutdown();

    State GetState() const { return mState.load(std::memory_order_acquire); }
    bool IsUpdateAvailable() const { return GetState() == State::UpdateAvailable; }
    const std::string& GetLatestVersion() const { return mLatest; }
    void OnFrame();

private:
    UpdateChecker() = default;
    ~UpdateChecker() = default;
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    void RunCheck();  
    void DeliverToast();  
    void PumpJoinChat();  

    std::atomic<State> mState{ State::Idle };
    std::string mLatest; 
    std::thread mWorker;
    std::atomic<bool> mWorkerRun{ false };
    bool mStopping = false;

    bool mToastDone = false;    
    bool mWasInWorld = false;   
    unsigned long long mJoinMs = 0;  
    bool mChatSent = false;    
    unsigned long long mChatTryMs = 0;
};
