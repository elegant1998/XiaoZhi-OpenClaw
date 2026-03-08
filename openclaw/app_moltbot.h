#ifndef APP_MOLTBOT_H
#define APP_MOLTBOT_H

#include <string>
#include <memory>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "web_socket.h"

struct SkillInfo {
    std::string name;
    std::string description;
    std::string schema_json;
};

class MoltbotClient {
private:
    std::shared_ptr<WebSocket> websocket_;
    bool is_connected_;
    bool is_handshake_done_;
    std::string token_;          // 网关的 OPENCLAW_GATEWAY_TOKEN
    std::string device_id_;      // 稳定的设备ID（如ESP32的MAC地址）
    std::string message_buffer_; // Buffer for accumulating text deltas

    // 辅助函数
    std::string generate_random_id(int len = 16);
    std::string get_device_id();

    void OnDataReceived(const char* data, int len);
    
    // Connection management
    TaskHandle_t connect_task_handle_ = nullptr;
    volatile bool running_ = false;
    std::string uri_;
    static void ConnectTask(void* arg);

    // Flow & Skills
    std::vector<SkillInfo> available_skills_;
    std::string available_skills_desc_ = "Loading skills...";
    std::string current_flow_id_;
    std::string pending_chain_json_; // JSON Array of remaining skills ["skill2", "skill3"]
    std::string flow_context_json_;  // JSON Object of accumulated context
    std::string pending_action_json_; // Stored action from simulation mode

    void SendRpcRequest(const std::string& method, const std::string& params_json = "{}");
    void ExecuteNextSkill();
    void ExecutePendingAction();
    void OnSkillComplete(const std::string& result_json);

public:
    MoltbotClient();
    ~MoltbotClient();
    static MoltbotClient& GetInstance();
    void Start(const std::string& host, int port, const std::string& gateway_token);
    void SendText(const std::string& text);
    void SendUserMessage(const std::string& message);
    void SendToWeChatAsync(const std::string& msg);
    
    // Skill Integration
    void FetchSkills();
    void ExecuteSkillChain(const std::string& chain_json, const std::string& params_json);
    std::string GetSkillsDescription() { return available_skills_desc_; }
    const std::vector<SkillInfo>& GetAvailableSkills() const { return available_skills_; }
};

#endif // APP_MOLTBOT_H