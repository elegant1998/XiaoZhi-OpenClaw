#include "app_moltbot.h"
#include "esp_log.h"
#include "cJSON.h"
#include "mcp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <string>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

#include "boards/common/board.h"
#include "application.h"
#include "app_wechatbinding.h"

static const char *TAG = "Moltbot";

// 清理 Markdown 并合并空白
static std::string clean_markdown_text(const char* in) {
    if (!in) return std::string();
    std::string s(in);
    const char remove_chars[] = "*_`~[]()<>#";
    for (char &c : s) {
        for (char r : remove_chars) {
            if (c == r) { c = ' '; break; }
        }
    }
    // normalize whitespace
    std::string out;
    bool last_space = false;
    for (char c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        if (c == ' ') {
            if (!last_space) { out += c; last_space = true; }
        } else { out += c; last_space = false; }
    }
    // trim
    size_t st = out.find_first_not_of(' ');
    if (st == std::string::npos) return std::string();
    size_t ed = out.find_last_not_of(' ');
    return out.substr(st, ed - st + 1);
}

// Async job for background splitting/speaking/forwarding.
struct SmartJob {
    char* text; // heap-allocated
    int listen_mode; 
    bool local_Speaking;
    int inter_delay_ms;
};

// single persistent worker and queue to serialize TTS tasks
static QueueHandle_t smart_job_queue = NULL;
static TaskHandle_t smart_worker_handle = NULL;

static void smart_worker_loop(void* arg) {
    (void)arg;
    SmartJob* job = NULL;
    for (;;) {
        if (xQueueReceive(smart_job_queue, &job, portMAX_DELAY) == pdTRUE) {
            if (!job) continue;
            
            if (job->text) {
                // Ensure text is safe to read
                // We trust job->text is null terminated from smart_split_and_send
                std::string cleaned = clean_markdown_text(job->text);

                if (!cleaned.empty()) {
                    // trim leading/trailing
                    size_t s = cleaned.find_first_not_of(' ');
                    size_t e = cleaned.find_last_not_of(' ');
                    if (s != std::string::npos && e != std::string::npos && e >= s) {
                         // Copy to avoid referencing temporary cleaned
                        std::string to_send = cleaned.substr(s, e - s + 1);
                        Application::GetInstance().SendTextPrompt(to_send, static_cast<ListeningMode>(job->listen_mode), job->local_Speaking); 
                    }
                }
                // Free text memory. 
                free(job->text);
            }

            // Free job struct
            free(job);
            job = NULL;
        }
    }
}

// Ensure queue and worker exist
static void ensure_smart_worker() {
    if (!smart_job_queue) {
        smart_job_queue = xQueueCreate(8, sizeof(SmartJob*));
    }
    if (!smart_worker_handle && smart_job_queue) {
        // Reduced stack size to save RAM, increased from previous 6144 but 8192 is safe.
        // If stack overflow is suspected, ensure this is enough.
        xTaskCreate(smart_worker_loop, "moltbot_tts", 8192, NULL, 5, &smart_worker_handle);
    }
}

// 非阻塞入口：复制文本到可用堆，并入队，由单个 worker 串行处理
static void smart_split_and_send(const char* text, int listen_mode = kListeningModeAutoStop, bool local_Speaking = true, int inter_delay_ms = 40) {
    if (!text || text[0] == '\0') return;
    ensure_smart_worker();
    if (!smart_job_queue) return;

    size_t len = strlen(text) + 1;
    
    // Allocate job struct from standard heap
    SmartJob* job = (SmartJob*)malloc(sizeof(SmartJob));
    if (!job) {
        ESP_LOGE(TAG, "Failed to allocate SmartJob");
        return;
    }

    // Allocate text buffer. Prefer SPIRAM for large text to save internal RAM.
    // Use calloc/malloc directly if you want simpler logic, or heap_caps_malloc for specific capabilities.
    // NOTE: heap_caps_malloc(..., MALLOC_CAP_SPIRAM) returns NULL if no SPIRAM.
    void* ptr = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(len); // Fallback to default
    }
    
    job->text = (char*)ptr;
    
    if (!job->text) {
        free(job);
        ESP_LOGE(TAG, "Failed to allocate job text");
        return;
    }
    
    // Copy and ensure null termination
    memcpy(job->text, text, len);
    job->text[len - 1] = '\0'; // Explicit safety

    job->listen_mode = listen_mode;
    job->local_Speaking = local_Speaking;
    job->inter_delay_ms = inter_delay_ms;

    BaseType_t ok = xQueueSend(smart_job_queue, &job, 0);
    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "smart_split_and_send queue full, dropping TTS");
        free(job->text);
        free(job);
    }
}
MoltbotClient::MoltbotClient() : is_connected_(false), is_handshake_done_(false), running_(false) {
    device_id_ = get_device_id();
}

MoltbotClient::~MoltbotClient() {
    running_ = false;
    if (websocket_) {
        websocket_->Close();
    }
}

MoltbotClient& MoltbotClient::GetInstance() {
    static MoltbotClient instance;
    return instance;
}

std::string MoltbotClient::generate_random_id(int len) {
    const char* chars = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string id;
    srand(xTaskGetTickCount());
    for (int i = 0; i < len; i++) {
        id += chars[rand() % strlen(chars)];
    }
    return id;
}

std::string MoltbotClient::get_device_id() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18] = {0};
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string("esp32_") + mac_str;
}

void MoltbotClient::Start(const std::string& host, int port, const std::string& gateway_token) {
    if (running_) {
        ESP_LOGW(TAG, "Already started");
        return;
    }
    
    token_ = gateway_token;
    ESP_LOGI(TAG, "Using device ID: %s", device_id_.c_str());

    // Generate a proper random device ID for each boot or persist it in NVS
    // Fix: "device identity mismatch" means server expects a different ID for this token/IP/session pair
    // or the 'id' field format is wrong. Let's try to sync device.id with client.instanceId concept if possible
    // or just use a pure random UUID like string which is what most clients do for ephemeral sessions.
    // However, OpenClaw expects stable IDs.
    // Let's try to make the device ID look like a proper UUID to avoid format validation issues if any,
    // although "mismatch" usually implies Auth token is bound to a DIFFERENT device ID.
    
    // ACTION: Reset device ID to be derived from MAC but formatted as UUID if needed, 
    // BUT the error is likely due to the AUTH TOKEN having been used with a DIFFERENT device ID before.
    // Since we are using "xiaozhi" token (from log), maybe it's bound.
    // Let's try to append a random suffix to device ID to force a new "device" registration 
    // OR we need to use a clean token.
    
    // Quick fix: Randomize device ID slightly to be treated as a new device
    // device_id_ = get_device_id() + "_" + generate_random_id(4);
    
    // Actually, let's keep it stable but ensure it matches what we send.
    // The log says we send: "id":"xiaozhi-esp32-s3-001" 
    // But get_device_id() returns "esp32_" + mac. 
    // WAIT! In OnDataReceived we HARDCODED "xiaozhi-esp32-s3-001"!
    // We should use self->device_id_ !

    std::string protocol = (port == 443) ? "wss" : "ws";
    uri_ = protocol + "://" + host + ":" + std::to_string(port) + "/";

    ESP_LOGI(TAG, "Connecting to OpenClaw Gateway: %s", uri_.c_str());

    running_ = true;
    xTaskCreate(ConnectTask, "moltbot_conn", 6144, this, 5, &connect_task_handle_);
}

void MoltbotClient::ConnectTask(void* arg) {
    MoltbotClient* self = (MoltbotClient*)arg;
    
    // 1. Wait for Network Interface availability
    NetworkInterface* network = nullptr;
    while (self->running_) {
        network = Board::GetInstance().GetNetwork();
        if (network) break;
        ESP_LOGI(TAG, "Waiting for network interface...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!self->running_) {
        vTaskDelete(NULL);
        return;
    }

    // 2. Initialize WebSocket
    self->websocket_ = std::make_shared<WebSocket>(network, 0);
    
    // Extract Origin
    std::string origin = self->uri_;
    size_t third_slash = origin.find('/', origin.find("://") + 3);
    if (third_slash != std::string::npos) {
        origin = origin.substr(0, third_slash);
    }
    self->websocket_->SetHeader("Origin", origin.c_str());

    self->websocket_->OnConnected([self]() {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        self->is_connected_ = true;
    });

    self->websocket_->OnDisconnected([self]() {
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        self->is_connected_ = false;
        self->is_handshake_done_ = false;
    });

    self->websocket_->OnData([self](const char* data, size_t len, bool binary) {
        if (!binary) {
            self->OnDataReceived(data, len);
        } else {
            // Treat binary as raw audio or Opus from OpenClaw
            // For MVP: assume it's playable directly via Application::PlaySound
            // But PlaySound expects std::string_view, which works with binary data too (it's just bytes)
            ESP_LOGI("Moltbot", "Received Binary Data: %d bytes", len);
            Application::GetInstance().PlaySound(std::string_view(data, len));
        }
    });

    self->websocket_->OnError([](int error) {
        ESP_LOGE(TAG, "WebSocket Error: %d", error);
    });
    
    // 3. Connection Loop
    while (self->running_) {
        if (!self->is_connected_) {
            ESP_LOGI(TAG, "Connecting to WebSocket...");
            if (self->websocket_->Connect(self->uri_.c_str())) {
                ESP_LOGI(TAG, "Handshake Success");
                // Don't fetch skills immediately, wait for protocol handshake (connect.challenge -> connect)
                // self->FetchSkills(); 
            } else {
                ESP_LOGW(TAG, "Connection failed, retrying in 5s...");
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    vTaskDelete(NULL);
}

void MoltbotClient::OnDataReceived(const char* data, int len) {
    //ESP_LOGI(TAG, "RX: %.*s", len, data);
    
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON (len=%d)", len);
        return;
    }

    // --- Check for JSON-RPC Response (Wrapped in Custom Protocol 'res') ---
    // The server returns responses like: {"type":"res", "id":"...", "ok":true, "payload": { "result": ... }}
    // OR {"type":"res", "id":"...", "ok":true, "payload": ...directly result...}
    // We need to inspect 'id' to route it.
    
    cJSON *typeItem = cJSON_GetObjectItem(root, "type");

    if (typeItem && cJSON_IsString(typeItem) && strcmp(typeItem->valuestring, "res") == 0) {
         cJSON* id_item = cJSON_GetObjectItem(root, "id");
         
         if (id_item && cJSON_IsString(id_item)) {
             std::string id_str = id_item->valuestring;
             
             // 1. Handle Skill List Response (or generic error for it)
             if (id_str.find("skill-list") != std::string::npos) {
                 cJSON* okItem = cJSON_GetObjectItem(root, "ok");
                 bool success = okItem && (cJSON_IsTrue(okItem) || (cJSON_IsNumber(okItem) && okItem->valueint == 1));
                 
                 std::string skills_summary = "";

                 if (success) {
                     cJSON* payload = cJSON_GetObjectItem(root, "payload");
                     if (payload) {
                         cJSON* skillsArr = cJSON_GetObjectItem(payload, "skills");
                         if (skillsArr && cJSON_IsArray(skillsArr)) {
                             // Clear previous skills list
                             MoltbotClient::GetInstance().available_skills_.clear();
                             
                             int count = cJSON_GetArraySize(skillsArr);
                             for (int i = 0; i < count; i++) {
                                 cJSON* item = cJSON_GetArrayItem(skillsArr, i);
                                 if (!item) continue;
                                 
                                 bool eligible = false;
                                 bool disabled = true;

                                 cJSON* el = cJSON_GetObjectItem(item, "eligible");
                                 if (el && cJSON_IsTrue(el)) eligible = true;
                                 
                                 cJSON* dis = cJSON_GetObjectItem(item, "disabled");
                                 if (dis && cJSON_IsFalse(dis)) disabled = false;

                                 // Optional: Filter by specific tags if needed
                                 
                                 if (eligible && !disabled) {
                                     cJSON* name = cJSON_GetObjectItem(item, "name");
                                     cJSON* desc = cJSON_GetObjectItem(item, "description");
                                     cJSON* schema = cJSON_GetObjectItem(item, "schema");
                                     
                                     if (name && cJSON_IsString(name)) {
                                         SkillInfo info;
                                         info.name = name->valuestring;
                                         if (desc && cJSON_IsString(desc)) info.description = desc->valuestring;
                                         if (schema) {
                                             char* schemaStr = cJSON_PrintUnformatted(schema);
                                             if (schemaStr) {
                                                 info.schema_json = schemaStr;
                                                 free(schemaStr);
                                             }
                                         }
                                         MoltbotClient::GetInstance().available_skills_.push_back(info);
                                     }
                                 }
                             }
                             
                             // Build compact summary list with Categories
                             skills_summary = "Capabilities: ";
                             for (const auto& s : MoltbotClient::GetInstance().available_skills_) {
                                 skills_summary += s.name + ", ";
                             }
                         }
                     }
                 } else {
                     skills_summary += "None loaded.";
                 }
                 
                 // Update the member variable for later use in lambda
                 MoltbotClient::GetInstance().available_skills_desc_ = skills_summary;

                 // Register the SINGLE gateway tool with the dynamic description
                 McpServer::GetInstance().AddTool(
                     "self.execute_task", 
                     ("Use this tool to execute task by skills. Available skills: " + skills_summary + ". You can directly provide the skill name and arguments.").c_str(),
                     PropertyList({
                         {"skill_name", kPropertyTypeString}, // Exact skill name from the list
                         {"arguments", kPropertyTypeString}   // JSON object string of arguments for the skill
                     }), 
                     [](const PropertyList& properties) -> ReturnValue {
                         std::string skill = "";
                         if (properties.Has("skill_name")) {
                             skill = properties["skill_name"].value<std::string>();
                         } else {
                             return PsramString("Error: skill_name is required.");
                         }

                         std::string args = "{}";
                         if (properties.Has("arguments")) {
                             args = properties["arguments"].value<std::string>();
                             
                             // Attempt to fix non-JSON arguments (e.g., "key: value, key2: value2")
                             // Check if it starts with '{' or '[' (likely JSON)
                             bool looks_like_json = false;
                             for (char c : args) {
                                if (isspace(c)) continue;
                                if (c == '{' || c == '[') looks_like_json = true;
                                break;
                             }
                             
                             if (!looks_like_json && !args.empty()) {
                                 ESP_LOGW("Moltbot", "Arguments not JSON, attempting heuristic parse: %s", args.c_str());
                                 cJSON* root = cJSON_CreateObject();
                                 std::string current_key, current_val;
                                 bool in_key = true;
                                 
                                 // Simple parser: split by ',' or newline, then split by ':' or '='
                                 // "action: send, recipient: x@x.com" or "source=internal_memory, query=..."
                                 
                                 // Pre-process: if we find keys like "query=" and the value contains commas (e.g. "10,000"), 
                                 // simple splitting by ',' breaks.
                                 // However, building a robust parser is hard.
                                 // Let's try to detect if a comma is followed by a "key=" pattern.
                                 
                                 std::stringstream ss(args);
                                 std::string segment;
                                 std::vector<std::string> segments;
                                 
                                 // Support both ',' and '&' as delimiters
                                 // We'll replace '&' with ',' first to use the existing logic? 
                                 // Or just write a custom splitter.
                                 
                                 std::string current_seg;
                                 for (char c : args) {
                                     if (c == ',' || c == '&' || c == '\n') {
                                         segments.push_back(current_seg);
                                         current_seg.clear();
                                     } else {
                                         current_seg += c;
                                     }
                                 }
                                 if (!current_seg.empty()) segments.push_back(current_seg);
                                 
                                 std::string pending_entry;
                                 
                                 for (const auto& seg : segments) {
                                     // Check if this segment starts a NEW key-value pair
                                     // (Must contain ':' or '=' somewhere valid)
                                     bool is_start_of_new = (seg.find('=') != std::string::npos) || (seg.find(':') != std::string::npos);
                                     
                                     if (is_start_of_new) {
                                         // If we have a pending entry accumulating, flush it now
                                         if (!pending_entry.empty()) {
                                             // flush previous
                                             size_t sep_pos = pending_entry.find(':');
                                             if (sep_pos == std::string::npos) sep_pos = pending_entry.find('=');
                                             if (sep_pos != std::string::npos) {
                                                 std::string k = pending_entry.substr(0, sep_pos);
                                                 std::string v = pending_entry.substr(sep_pos + 1);
                                                 // trim
                                                 auto trim = [](std::string& s) {
                                                     if (s.empty()) return;
                                                     size_t first = s.find_first_not_of(" \t\n\r");
                                                     if (first == std::string::npos) { s.clear(); return; }
                                                     size_t last = s.find_last_not_of(" \t\n\r");
                                                     s = s.substr(first, (last - first + 1));
                                                 };
                                                 trim(k); trim(v);
                                                 if (!k.empty()) cJSON_AddStringToObject(root, k.c_str(), v.c_str());
                                             }
                                         }
                                         // Start new
                                         pending_entry = seg;
                                     } else {
                                         // Continuation of value (e.g. "10,000")
                                         if (!pending_entry.empty()) {
                                             pending_entry += "," + seg;
                                         } else {
                                             // Stray value at start? Treat as default description?
                                             pending_entry = seg;
                                         }
                                     }
                                 }
                                 
                                 // Flush final
                                 if (!pending_entry.empty()) {
                                     size_t sep_pos = pending_entry.find(':');
                                     if (sep_pos == std::string::npos) sep_pos = pending_entry.find('=');
                                     if (sep_pos != std::string::npos) {
                                         std::string k = pending_entry.substr(0, sep_pos);
                                         std::string v = pending_entry.substr(sep_pos + 1);
                                         auto trim = [](std::string& s) {
                                             if (s.empty()) return;
                                             size_t first = s.find_first_not_of(" \t\n\r");
                                             if (first == std::string::npos) { s.clear(); return; }
                                             size_t last = s.find_last_not_of(" \t\n\r");
                                             s = s.substr(first, (last - first + 1));
                                         };
                                         trim(k); trim(v);
                                         if (!k.empty()) cJSON_AddStringToObject(root, k.c_str(), v.c_str());
                                     }
                                 }

                                 char* new_json = cJSON_PrintUnformatted(root);
                                 if (new_json) {
                                     args = new_json;
                                     free(new_json);
                                 }
                                 cJSON_Delete(root);
                             }
                         }

                         // 1. Find exact skill
                         bool found = false;
                         for (const auto& s : MoltbotClient::GetInstance().available_skills_) {
                             if (s.name == skill) {
                                 found = true;
                                 break;
                             }
                         }
                         
                         if (!found) {
                             // Fallback: Try fuzzy search and return helpful error
                             std::string suggestions = "";
                             std::string q_lower = skill;
                             std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);
                             
                             for (const auto& s : MoltbotClient::GetInstance().available_skills_) {
                                 std::string n_lower = s.name;
                                 std::transform(n_lower.begin(), n_lower.end(), n_lower.begin(), ::tolower);
                                 if (n_lower.find(q_lower) != std::string::npos || q_lower.find(n_lower) != std::string::npos) {
                                     suggestions += s.name + " (" + s.description + "); ";
                                 }
                             }
                             
                             if (!suggestions.empty()) {
                                 return PsramString(("Skill '" + skill + "' not found. Did you mean: " + suggestions).c_str());
                             }
                             return PsramString(("Skill '" + skill + "' not found in available skills list.").c_str());
                         }

                         // 2. Execute
                         cJSON* chain = cJSON_CreateArray();
                         cJSON_AddItemToArray(chain, cJSON_CreateString(skill.c_str()));
                         char* chain_str = cJSON_PrintUnformatted(chain);
                         
                         MoltbotClient::GetInstance().ExecuteSkillChain(chain_str, args);
                         free(chain_str);
                         cJSON_Delete(chain);

                         return PsramString("Command accepted and execution started.");
                     }
                 );

                 cJSON_Delete(root);
                 return;
             } // Close if (id_str.find("skill-list"))

             
             // 2. Handle Skill Run Response
             else if (id_str.find("skill-run") != std::string::npos) {
                 cJSON* payload = cJSON_GetObjectItem(root, "payload");
                 if (payload) {
                     char* res = cJSON_PrintUnformatted(payload);
                     std::string res_str = res;
                     free(res);
                     OnSkillComplete(res_str);
                 }
                 cJSON_Delete(root);
                 return;
             }
         }
         
         // 3. Handle Generic Handshake (already checked above logic, but let's reinforce)
         // The previous "if (type==res)" block handles generic handshake if ID is random
         // But "skills.list" also has type=res.
         // Effectively, if we are here, we are handling specific IDs.
         // If ID is NOT skill-list or skill-run, it falls through to the generic handshake check below
         
         cJSON *okItem = cJSON_GetObjectItem(root, "ok");
         if (okItem && (cJSON_IsTrue(okItem) || (cJSON_IsNumber(okItem) && okItem->valueint == 1))) {
            if (!is_handshake_done_) {
                is_handshake_done_ = true;
                ESP_LOGI(TAG, "Protocol Handshake Complete (type=res). Fetching Skills...");
                FetchSkills();
            }
         }
         cJSON_Delete(root);
         return;
    }
    
    // Remove old "result" check as it's not valid for this protocol wrapper
    /* 
    if (cJSON_GetObjectItem(root, "result")) { ... }
    */

    cJSON *event = cJSON_GetObjectItem(root, "event");
    if (event && cJSON_IsString(event) && strcmp(event->valuestring, "connect.challenge") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        cJSON *nonce = NULL; 
        cJSON *ts = NULL;
        if(payload) {
            nonce = cJSON_GetObjectItem(payload, "nonce");
            ts = cJSON_GetObjectItem(payload, "ts");
        }
        
        if (nonce && cJSON_IsString(nonce)) {
            ESP_LOGI(TAG, "Handling Challenge Nonce: %s", nonce->valuestring);
            
            std::string current_token = token_.empty() ? "xiaozhi2026" : token_;
            std::string nonce_str = nonce->valuestring;
            double signed_at = ts ? ts->valuedouble : (double)(time(NULL) * 1000); 
            
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "type", "req");
            
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "msg-%u", (unsigned int)esp_random());
            cJSON_AddStringToObject(resp, "id", id_buf);
            
            cJSON_AddStringToObject(resp, "method", "connect");
            
            cJSON *params = cJSON_CreateObject();
            cJSON_AddNumberToObject(params, "minProtocol", 3);
            cJSON_AddNumberToObject(params, "maxProtocol", 3);
            
            cJSON *client = cJSON_CreateObject();
            cJSON_AddStringToObject(client, "id", "node-host");
            cJSON_AddStringToObject(client, "version", "2.0.0");
            cJSON_AddStringToObject(client, "platform", "esp32");
            cJSON_AddStringToObject(client, "mode", "node");
            cJSON_AddItemToObject(params, "client", client);
            
            cJSON_AddStringToObject(params, "role", "node");
            cJSON_AddItemToObject(params, "scopes", cJSON_CreateArray());
            
            cJSON *caps = cJSON_CreateArray();
            cJSON_AddItemToArray(caps, cJSON_CreateString("audio_input"));
            cJSON_AddItemToArray(caps, cJSON_CreateString("audio_output"));
            cJSON_AddItemToObject(params, "caps", caps);
            
            cJSON_AddItemToObject(params, "commands", cJSON_CreateArray());
            cJSON_AddItemToObject(params, "permissions", cJSON_CreateObject());
            
            cJSON *auth = cJSON_CreateObject();
            cJSON_AddStringToObject(auth, "token", current_token.c_str());
            cJSON_AddItemToObject(params, "auth", auth);
            
            cJSON_AddStringToObject(params, "locale", "en-US");
            cJSON_AddStringToObject(params, "userAgent", "openclaw-cli/2.0.0");

            // cJSON *device = cJSON_CreateObject();
            // cJSON_AddStringToObject(device, "id", device_id_.c_str());
            // cJSON_AddStringToObject(device, "nonce", nonce_str.c_str());
            // cJSON_AddNumberToObject(device, "signedAt", signed_at);
            // // Schema requires these fields, providing dummies for allowInsecureAuth mode
            // cJSON_AddStringToObject(device, "publicKey", "dummy_pk");
            // cJSON_AddStringToObject(device, "signature", "dummy_sig");

            // cJSON_AddItemToObject(params, "device", device);
            
            cJSON_AddItemToObject(resp, "params", params);
            
            char *msg = cJSON_PrintUnformatted(resp);
            ESP_LOGI(TAG, "Sending Connect Request: %s", msg);
            
            vTaskDelay(pdMS_TO_TICKS(50));
            SendText(msg);
            
            free(msg);
            cJSON_Delete(resp);
        }
    } else if (event && cJSON_IsString(event) && strcmp(event->valuestring, "message") == 0) {
        // Handle incoming text message from OpenClaw (via Audio or Text)
        // Expected payload: { "text": "Hello world" } or similar
        // For MVP, if we get text, we use XiaoZhi's TTS Service to speak it out.
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            cJSON *text = cJSON_GetObjectItem(payload, "text");
                if (text && cJSON_IsString(text)) {
                ESP_LOGI(TAG, "Received Message from OpenClaw: %s", text->valuestring);
                // Use smart splitter to speak and forward (async)
                smart_split_and_send(text->valuestring, kListeningModeAutoStop, true);
            }
        }
    } else if (event && cJSON_IsString(event) && strcmp(event->valuestring, "agent") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            cJSON *data = cJSON_GetObjectItem(payload, "data");
            if (data) {
                // Check for 'lifecycle' end phase to flush buffer
                cJSON *phase = cJSON_GetObjectItem(data, "phase");
                if (phase && cJSON_IsString(phase) && strcmp(phase->valuestring, "end") == 0) {
                     if (!message_buffer_.empty()) {
                         ESP_LOGI(TAG, "Flushing final buffer: %s", message_buffer_.c_str());
                         smart_split_and_send(message_buffer_.c_str(), kListeningModeAutoStop, true);
                         message_buffer_.clear();
                     }
                     return;
                }

                // Try to use delta if available for streaming
                cJSON *delta = cJSON_GetObjectItem(data, "delta");
                if (delta && cJSON_IsString(delta) && strlen(delta->valuestring) > 0) {
                    message_buffer_ += delta->valuestring;
                    
                    // Logic to reduce TTS tasks: Only speak on punctuation or significant length
                    // English: . ? ! \n
                    // Chinese: 。 ？ ！
                    const std::string& buf = message_buffer_;
                    size_t len = buf.length();
                    bool should_flush = false;
                    
                    if (len > 0) {
                        char last = buf.back();
                        // Standard English punctuation
                        if (last == '.' || last == '?' || last == '!' || last == '\n') should_flush = true;
                        
                        // Chinese punctuation (multi-byte checks)
                        // UTF-8: 。= E3 80 82, ？= EF BC 9F, ！= EF BC 81
                        if (len >= 3) {
                            const char* p = buf.c_str() + len - 3;
                            if (memcmp(p, "\xe3\x80\x82", 3) == 0) should_flush = true; // 。
                            else if (memcmp(p, "\xef\xbc\x9f", 3) == 0) should_flush = true; // ？
                            else if (memcmp(p, "\xef\xbc\x81", 3) == 0) should_flush = true; // ！
                        }

                        // Safety flush if buffer gets too large (e.g. 100 chars without punctuation)
                        if (len > 100) should_flush = true;
                    }

                    if (should_flush) {
                        ESP_LOGI(TAG, "Speaking buffered Text: %s", message_buffer_.c_str());
                        smart_split_and_send(message_buffer_.c_str(), kListeningModeAutoStop, true);
                        message_buffer_.clear();
                    }
                    
                } else {
                    // Fallback to full text (if delta is missing but text exists - usually specific events)
                    // ... existing fallback ...
                    cJSON *text = cJSON_GetObjectItem(data, "text");
                        if (text && cJSON_IsString(text)) {
                        /* Only use full text if we haven't been explicitely buffering deltas for this message?
                           Actually, standard protocol: either delta OR text.
                           But sometimes 'agent' event has both.
                           If message_buffer_ is not empty, we are in streaming mode.
                           If we get 'text', it might be the FULL text at the end?
                           Let's trust 'delta' if buffers are used.
                        */
                        if (message_buffer_.empty()) {
                            ESP_LOGI(TAG, "Received Agent Text: %s", text->valuestring);
                            smart_split_and_send(text->valuestring, kListeningModeAutoStop, true);
                        }
                    }
                }
            }
        }
    } else {
        // Check for node.event method (New Protocol)
        cJSON *method = cJSON_GetObjectItem(root, "method");
        if (method && cJSON_IsString(method) && strcmp(method->valuestring, "node.event") == 0) {
            cJSON *params = cJSON_GetObjectItem(root, "params");
            if (params) {
                // 1. Try payloadJSON (Double-encoded)
                cJSON *payloadJSON = cJSON_GetObjectItem(params, "payloadJSON");
                if (payloadJSON && cJSON_IsString(payloadJSON)) {
                    cJSON *inner = cJSON_Parse(payloadJSON->valuestring);
                    if (inner) {
                        cJSON *text = cJSON_GetObjectItem(inner, "text");
                        if (text && cJSON_IsString(text)) {
                            ESP_LOGI(TAG, "Received Node Event (JSON): %s", text->valuestring);
                            smart_split_and_send(text->valuestring, kListeningModeAutoStop, true);
                        }
                        cJSON_Delete(inner);
                    }
                }
                // 2. Try direct payload object
                else {
                    cJSON *payload = cJSON_GetObjectItem(params, "payload");
                    if (payload) {
                        cJSON *text = cJSON_GetObjectItem(payload, "text");
                        if (text && cJSON_IsString(text)) {
                            ESP_LOGI(TAG, "Received Node Event (Direct): %s", text->valuestring);
                            smart_split_and_send(text->valuestring, kListeningModeAutoStop, true);
                        }
                    }
                }
            }
        }
    }

    cJSON_Delete(root);

}

void MoltbotClient::SendToWeChatAsync(const std::string& msg) {
    return; // Disable WeChat sending for now

    
    if (msg.empty()) return;
    std::string* msg_ptr = new std::string(msg);
    // Use a separate task for HTTP request to avoid Stack Overflow in WebSocket Task
    // Stack depth of 8192 is safer for TLS/HTTP operations
    // Use MALLOC_CAP_SPIRAM to save internal SRAM
    xTaskCreateWithCaps([](void* arg) {
        std::string* s = (std::string*)arg;
        WeChatBindingManager::GetInstance().SendMessage(*s);
        delete s;
        vTaskDelete(NULL);
    }, "wx_async", 8192, msg_ptr, 5, NULL, MALLOC_CAP_SPIRAM);
}

void MoltbotClient::SendText(const std::string& text) {
    if (is_connected_ && websocket_) {
        ESP_LOGI(TAG, "WS TX: %s", text.c_str());
        if (websocket_->Send(text)) {
            // Success
        } else {
            ESP_LOGE(TAG, "Failed to send text");
        }
    } else {
        ESP_LOGE(TAG, "Cannot send text - not connected");
    }
}

void MoltbotClient::SendUserMessage(const std::string& message) {
    // Escape quotes for JSON payload string
    std::string escaped_msg;
    for (char c : message) {
        if (c == '"') escaped_msg += "\\\"";
        else if (c == '\\') escaped_msg += "\\\\";
        else escaped_msg += c;
    }

    // 1. Inner Payload: {"text": "...", "sessionKey": "main"}
    // This replicates the structure expected by 'voice.transcript' event handler
    std::string payload_inner = "{\"text\":\"" + escaped_msg + "\",\"sessionKey\":\"main\"}";

    // 2. Escape Inner Payload for Outer JSON string value
    std::string escaped_payload;
    for (char c : payload_inner) {
        if (c == '"') escaped_payload += "\\\"";
        else if (c == '\\') escaped_payload += "\\\\";
        else escaped_payload += c;
    }

    // 3. Generate Request ID
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "req-%u", (unsigned int)esp_random());

    // 4. Outer Request: node.event(voice.transcript)
    // {
    //   "type": "req",
    //   "id": "...",
    //   "method": "node.event",
    //   "params": {
    //     "event": "voice.transcript",
    //     "payloadJSON": "..."
    //   }
    // }
    std::string json = "{\"type\":\"req\",\"id\":\"" + std::string(id_buf) + "\",\"method\":\"node.event\",\"params\":{\"event\":\"voice.transcript\",\"payloadJSON\":\"" + escaped_payload + "\"}}";
    
    // Check for local execution command
    std::string lower_msg = message;
    std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(), ::tolower);
    if ((lower_msg.find("execute") != std::string::npos || lower_msg.find("run") != std::string::npos || lower_msg.find("confirm") != std::string::npos || lower_msg.find("执行") != std::string::npos || lower_msg.find("确认") != std::string::npos) && !pending_action_json_.empty()) {
        ESP_LOGI(TAG, "Intercepted Execute Command. Running pending action...");
        ExecutePendingAction();
        return;
    }
    
    SendText(json);
}

void MoltbotClient::ExecutePendingAction() {
    if (pending_action_json_.empty()) return;
    
    ESP_LOGI(TAG, "Executing Pending Action: %s", pending_action_json_.c_str());
    
    cJSON *action = cJSON_Parse(pending_action_json_.c_str());
    if (action) {
        cJSON *skill = cJSON_GetObjectItem(action, "skill");
        cJSON *args = cJSON_GetObjectItem(action, "args");
        
        if (skill && cJSON_IsString(skill)) {
            // Send skills.run request
            cJSON *rpc_params = cJSON_CreateObject();
            cJSON_AddStringToObject(rpc_params, "skill", skill->valuestring);
            if (args) {
                cJSON_AddItemToObject(rpc_params, "args", cJSON_Duplicate(args, 1));
            }
            
            char *rpc_params_str = cJSON_PrintUnformatted(rpc_params);
            SendRpcRequest("skills.run", rpc_params_str);
            free(rpc_params_str);
            cJSON_Delete(rpc_params);
            
            smart_split_and_send("正在执行操作...", kListeningModeAutoStop, true);
        }
        cJSON_Delete(action);
    }
    
    pending_action_json_.clear();
}

// --- Skill Integration Implementation ---

void MoltbotClient::SendRpcRequest(const std::string& method, const std::string& params_json) {
    cJSON *req = cJSON_CreateObject();
    
    // --- Protocol Fix: Use Custom Protocol for RPC wrapper ---
    // Instead of raw JSON-RPC 2.0 at root, wrap it in type=req envelope
    
    cJSON_AddStringToObject(req, "type", "req");
    
    // Generate ID with prefix to identify response type
    std::string prefix = "req-";
    if (method == "skills.status") prefix = "skill-list-";
    if (method == "skills.run") prefix = "skill-run-";
    
    char id_buf[64];
    snprintf(id_buf, sizeof(id_buf), "%s%u", prefix.c_str(), (unsigned int)esp_random());
    cJSON_AddStringToObject(req, "id", id_buf); // Top level ID
    
    cJSON_AddStringToObject(req, "method", method.c_str());
    
    // params is at top level in this protocol
    if (!params_json.empty()) {
        cJSON *params = cJSON_Parse(params_json.c_str());
        if (params) {
            cJSON_AddItemToObject(req, "params", params);
        } else {
             // If parse fails, add empty object
             cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
        }
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }
    
    // Add token to the top-level request object (or inside params if required, but usually top-level for auth)
    // Based on user request to add "token": "..." 
    //{"type":"res","id":"skill-list-2359807672","ok":false,"error":{"code":"INVALID_REQUEST","message":"invalid request frame: at root: unexpected property 'token'"}}

    /*if (!token_.empty()) {
        cJSON_AddStringToObject(req, "token", token_.c_str());
    }*/
    
    char *msg = cJSON_PrintUnformatted(req);
    SendText(msg);
    free(msg);
    cJSON_Delete(req);
}

void MoltbotClient::FetchSkills() {
    SendRpcRequest("skills.status", "{}");
}

void MoltbotClient::ExecuteSkillChain(const std::string& chain_json, const std::string& params_json) {
    pending_chain_json_ = chain_json;
    flow_context_json_ = params_json;
    current_flow_id_ = generate_random_id(8);
    ESP_LOGI(TAG, "Starting Flow %s with chain: %s", current_flow_id_.c_str(), chain_json.c_str());
    ExecuteNextSkill();
}

void MoltbotClient::ExecuteNextSkill() {
    cJSON *chain = cJSON_Parse(pending_chain_json_.c_str());
    if (!chain || !cJSON_IsArray(chain) || cJSON_GetArraySize(chain) == 0) {
        ESP_LOGI(TAG, "Flow %s Complete. Final Context: %s", current_flow_id_.c_str(), flow_context_json_.c_str());
        // Notify User (Simulated notification)
        std::string reply = "老板，任务搞定了！结果是：" + flow_context_json_; // Simplify context dump
        smart_split_and_send(reply.c_str()); // Optional: Speak it
        
        // Notify MCP via node.event if needed, or just let it be.
        if (chain) cJSON_Delete(chain);
        return;
    }
    
    // Get first skill
    cJSON *firstItem = cJSON_GetArrayItem(chain, 0);
    std::string skill_name_extracted;
    cJSON *skill_args_extracted = NULL;

    if (firstItem) {
        if (cJSON_IsString(firstItem)) {
            skill_name_extracted = firstItem->valuestring;
        } else if (cJSON_IsObject(firstItem)) {
            cJSON *sInfo = cJSON_GetObjectItem(firstItem, "skill");
            if (sInfo && cJSON_IsString(sInfo)) {
                skill_name_extracted = sInfo->valuestring;
            }
            cJSON *aInfo = cJSON_GetObjectItem(firstItem, "args");
            if (cJSON_IsObject(aInfo)) {
                skill_args_extracted = cJSON_Duplicate(aInfo, 1);
            } else if (cJSON_IsString(aInfo)) {
                // Handle JSON-encoded args string
                skill_args_extracted = cJSON_Parse(aInfo->valuestring);
            }
        }
    }
    
    if (skill_name_extracted.empty()) {
        if (chain) cJSON_Delete(chain);
        if (skill_args_extracted) cJSON_Delete(skill_args_extracted);
        return;
    }
    
    // Remove it from pending
    cJSON_DeleteItemFromArray(chain, 0);
    // Reuse variable name to avoid double free or issues
    char *new_chain_str = cJSON_PrintUnformatted(chain);
    pending_chain_json_ = new_chain_str;
    free(new_chain_str);
    cJSON_Delete(chain);
    
    ESP_LOGI(TAG, "Flow %s Executing Skill: %s", current_flow_id_.c_str(), skill_name_extracted.c_str());
    
    // Calls skills.run
    // Params: { "skill": "name", "args": { ...context... } }
    cJSON *rpc_params = cJSON_CreateObject();
    cJSON_AddStringToObject(rpc_params, "skill", skill_name_extracted.c_str());
    
    cJSON *merged_args = cJSON_CreateObject();
    
    // 1. Add args from chain definition (if any)
    if (skill_args_extracted) {
        cJSON *child = skill_args_extracted->child;
        while(child) {
            // FIX: If this arg is "attachments" and it's an array of strings that look like filenames,
            // but we have a "result" from previous context that is a "ref:...", 
            // the LLM likely hallucinated the filename expecting it to be created.
            // We should trust the 'result' (ref) more than the 'attachments' (hallucinated filename).
            // So if we see 'attachments' here, we might want to drop it IF we also have context
            // that will be merged in step 2.
            
            // However, we can't see step 2 yet. 
            // Strategy: Add it, but let step 2 overwrite if collision.
            // PROBLEM: 'attachments' key might not collide with 'result' key from previous step.
            // Previous step output is {"result": "ref:..."}
            // Chain args is {"attachments": ["plan.pdf"]}
            
            // So we will end up with BOTH {"result": "ref...", "attachments": ["plan.pdf"]}
            // The Skill (imap-smtp-email) sees "attachments" and tries to load "plan.pdf".
            
            // FIX: Explicitly remove "attachments" from chain args IF it looks like a hallucinated filename
            // and we are arguably in a chain. 
            // OR: simpler, just always remove "attachments" from chain args?
            // No, user might validly attach a static file.
            
            // Heuristic: If we are not the first skill in chain (implied by execution flow), 
            // and arg is 'attachments', we act suspicious.
            // But here we are in ExecuteNextSkill, so we don't know index easily unless tracked.
            
            // Let's modify the merging logic below instead.
            
            cJSON_AddItemToObject(merged_args, child->string, cJSON_Duplicate(child, 1));
            child = child->next;
        }
        cJSON_Delete(skill_args_extracted);
    }
    
    // 2. Merge current flow context
    cJSON *ctx = NULL;
    
    if (!flow_context_json_.empty()) {
        ctx = cJSON_Parse(flow_context_json_.c_str());
        if (ctx) {
            // Check for previous result ref
            /*
            cJSON* res_item = cJSON_GetObjectItem(ctx, "result");
            if (res_item && cJSON_IsString(res_item) && strstr(res_item->valuestring, "ref:result-")) {
                has_ref_result = true;
            }
            */

            cJSON *child = ctx->child;
            while(child) {
                // Safe merge: remove existing key in args if present, then add from context
                if (child->string) {
                    if (cJSON_HasObjectItem(merged_args, child->string)) {
                         cJSON_DeleteItemFromObject(merged_args, child->string);
                    }
                    cJSON_AddItemToObject(merged_args, child->string, cJSON_Duplicate(child, 1));
                }
                child = child->next;
            }
        }
    }
    
    // CRITICAL FIX: If we have a cached result ref from previous step,
    // we MUST remove any speculative "attachments" args that came from the static chain definition
    // because that's likely a hallucination of the file name.
    // The previous skill's output (the ref) is the real attachment source.
    /*
    if (has_ref_result) {
         if (cJSON_HasObjectItem(merged_args, "attachments")) {
             ESP_LOGW(TAG, "Conflict resolution: Removing speculative 'attachments' in favor of 'result' ref.");
             cJSON_DeleteItemFromObject(merged_args, "attachments");
         }
    }
    */
    
    if (ctx) cJSON_Delete(ctx);
    
    cJSON_AddItemToObject(rpc_params, "args", merged_args);
    
    // Final check for conflict: if 'result' exists (ref) AND 'attachments' exists (hallucination)
    // we drop 'attachments'. We do it here just in case.
    cJSON* res_item = cJSON_GetObjectItem(merged_args, "result");
    if (res_item && cJSON_IsString(res_item) && strstr(res_item->valuestring, "ref:result-")) {
          if (cJSON_HasObjectItem(merged_args, "attachments")) {
               ESP_LOGW(TAG, "Conflict resolution (Final): Removing speculative 'attachments' in favor of 'result' ref.");
               cJSON_DeleteItemFromObject(merged_args, "attachments");
          }
    }
    
    char *rpc_params_str = cJSON_PrintUnformatted(rpc_params);
    SendRpcRequest("skills.run", rpc_params_str);
    free(rpc_params_str);
    cJSON_Delete(rpc_params);
}

void MoltbotClient::OnSkillComplete(const std::string& result_json) {
    ESP_LOGI(TAG, "Skill Complete. Result: %s", result_json.c_str());
    
    // Merge result into context
    // Assume result is a JSON object. We merge it into flow_context_json_.
    cJSON *current_ctx = cJSON_Parse(flow_context_json_.c_str());
    if (!current_ctx) current_ctx = cJSON_CreateObject();
    
    cJSON *new_result = cJSON_Parse(result_json.c_str());
    if (new_result && cJSON_IsObject(new_result)) {
        // Simple merge: iterate and replace/add
        cJSON *child = new_result->child;
        while (child) {
            cJSON_DeleteItemFromObject(current_ctx, child->string); // Remove verify existence
            cJSON_AddItemToObject(current_ctx, child->string, cJSON_Duplicate(child, 1));
            child = child->next;
        }
    }
    if (new_result) cJSON_Delete(new_result);
    
    char *new_ctx_str = cJSON_PrintUnformatted(current_ctx);
    flow_context_json_ = new_ctx_str;
    free(new_ctx_str);
    cJSON_Delete(current_ctx);

    // If result is large text (like markdown prompt), speak out a summary
    cJSON *res_json = cJSON_Parse(result_json.c_str());
    if (res_json) {
        cJSON *result_text = cJSON_GetObjectItem(res_json, "result");
        if (result_text && cJSON_IsString(result_text)) {
            const char* text_ptr = result_text->valuestring;
            
            // Check for SUGGESTED_ACTION
            const char* action_marker = "SUGGESTED_ACTION:";
            char* found = strstr((char*)text_ptr, action_marker);
            
            // Try to parse the result string itself as JSON first (for clean success/error messages)
            bool handled_as_json = false;
            cJSON* inner_json = cJSON_Parse(text_ptr);
            if (inner_json) {
                if (cJSON_IsObject(inner_json)) {
                    // Check for standard success/error pattern
                    cJSON* success_item = cJSON_GetObjectItem(inner_json, "success");
                    cJSON* error_item = cJSON_GetObjectItem(inner_json, "error");
                    cJSON* msg_item = cJSON_GetObjectItem(inner_json, "message");
                    
                    if (success_item && cJSON_IsTrue(success_item)) {
                        ESP_LOGI(TAG, "Inner JSON indicates success");
                        std::string speak_msg = "操作成功";
                        if (msg_item && cJSON_IsString(msg_item)) {
                            speak_msg = msg_item->valuestring;
                        }
                        smart_split_and_send(speak_msg.c_str(), kListeningModeAutoStop, true);
                        handled_as_json = true;
                    } else if (error_item) {
                        ESP_LOGI(TAG, "Inner JSON indicates error");
                        std::string err_msg = "操作失败";
                        if (cJSON_IsString(error_item)) {
                            err_msg += ": ";
                            err_msg += error_item->valuestring;
                        } else if (cJSON_IsObject(error_item)) {
                            cJSON* code = cJSON_GetObjectItem(error_item, "code");
                            cJSON* msg = cJSON_GetObjectItem(error_item, "message");
                            if (msg && cJSON_IsString(msg)) {
                                err_msg += ": ";
                                err_msg += msg->valuestring;
                            }
                        }
                        smart_split_and_send(err_msg.c_str(), kListeningModeAutoStop, true);
                        handled_as_json = true;
                    }
                }
                cJSON_Delete(inner_json);
            }

            if (found) {
                char* json_start = found + strlen(action_marker);
                // Skip whitespace
                while (*json_start && (*json_start == ' ' || *json_start == '\n' || *json_start == '\r' || *json_start == '\t')) {
                    json_start++;
                }
                
                // Parse the action JSON
                cJSON* action_obj = cJSON_Parse(json_start);
                if (action_obj) {
                    char* action_str = cJSON_PrintUnformatted(action_obj);
                    pending_action_json_ = action_str;
                    free(action_str);
                    cJSON_Delete(action_obj);
                    ESP_LOGI(TAG, "Captured Pending Action: %s", pending_action_json_.c_str());
                    
                    // Notify User
                    smart_split_and_send("收到建议操作，请说“执行”来确认。", kListeningModeAutoStop, true);
                    
                    // Return early so we don't speak the raw JSON text as part of the output
                    // (But we might want to speak the text BEFORE the action...)
                    // For now, let's just speak the part before the action?
                    // The server usually puts the explanation first.
                    
                    // Calculate length of text before action
                    int prefix_len = found - text_ptr;
                    if (prefix_len > 0) {
                        std::string prefix(text_ptr, prefix_len);
                        smart_split_and_send(prefix.c_str(), kListeningModeAutoStop, true);
                    }
                    
                    cJSON_Delete(res_json);
                    return; 
                }
            }

            size_t len = strlen(text_ptr);
            if (len > 200) {
                 // It's a long content (likely a guide or article), just notify user
                 // Or we could try to send it back to LLM for summarization?
                 // For now, simpler: Just tell user we found something.
                 std::string prompt = "Executed skill and retrieved content. Length: " + std::to_string(len);
                 ESP_LOGI(TAG, "Skill content too long to speak direct. %s", prompt.c_str());
                 if (!handled_as_json) {
                    smart_split_and_send("Skill execution finished. I received the workflow guide.", kListeningModeAutoStop, true);
                 }
            } else {
                 if (!handled_as_json) {
                    if (strncmp(result_text->valuestring, "ref:", 4) == 0) {
                        smart_split_and_send("内容已生成并保存到服务器。", kListeningModeAutoStop, true);
                    } else {
                        smart_split_and_send(result_text->valuestring, kListeningModeAutoStop, true);
                    }
                 }
            }
        }
        cJSON_Delete(res_json);
    }
    
    // Run next
    ExecuteNextSkill();
}
