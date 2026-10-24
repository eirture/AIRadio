/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>

#include "application.h"
#include "board.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "MCP"

McpServer::McpServer() {}

McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList& properties) -> ReturnValue {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto& theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr) {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ToolResult {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
  auto &intercom = Application::GetInstance().GetIntercom();
  AddTool(
      "self.intercom.list_rooms",
      "查询本机可以加入的对讲房间列表。用户问‘当前有哪些房间’‘有哪些对讲群’"
      "‘列出房间和加入码’时调用。返回服务端实际房间名称、稳定ID、六位加入码、"
      "是否当前房间和是否默认房间。逐位朗读加入码并保留前导零；默认房间没有加入"
      "码。"
      "只列出开放、本机受邀或本机创建的有效房间。查询失败时说明失败原因，不要编"
      "造列表。",
      PropertyList(), [&intercom](const PropertyList &) -> ReturnValue {
        return intercom.ListRooms();
      });
  AddTool("self.intercom.get_current_room",
          "查询本机当前所在的对讲房间，返回 room_name（房间名称）。"
          "用户问‘我在哪个房间’‘当前对讲群是什么’‘现在在哪个频道’‘查看当前房间’"
          "时调用。"
          "这里的房间、群组、频道均指设备语音对讲房间。不要猜测房间名称，应根据"
          "工具结果回答。",
          PropertyList(), [&intercom](const PropertyList &) -> ReturnValue {
            cJSON *j = cJSON_CreateObject();
            cJSON_AddStringToObject(j, "room_name", intercom.RoomName());
            return j;
          });
  AddTool("self.intercom.create_room",
          "创建新的语音对讲房间。用户说‘创建房间’‘建个对讲群’‘新建频道’"
          "‘创建一个叫调度组的房间并邀请003和004’时调用。"
          "name 为用户指定的房间名称，必填；未提供名称时先询问，不要自行编造。"
          "invite_device_ids "
          "为邀请设备ID字符串，多个ID用英文逗号分隔，例如003,004；"
          "保留ID前导零。不指定邀请对象时传空字符串，表示开放加入，不能理解成自"
          "动拉入所有设备。"
          "创建者自动进入新房间；被邀请设备自行决定是否加入。"
          "当前返回true仅表示创建请求已发送，不代表服务端已确认创建成功；false"
          "表示请求未发出。",
          PropertyList({Property("name", kPropertyTypeString),
                        Property("invite_device_ids", kPropertyTypeString,
                                 std::string())}),
          [&intercom](const PropertyList &p) -> ReturnValue {
            std::vector<std::string> ids;
            auto raw = p["invite_device_ids"].value<std::string>();
            size_t start = 0;
            while (start < raw.size()) {
              auto end = raw.find(',', start);
              ids.push_back(raw.substr(
                  start, end == std::string::npos ? end : end - start));
              if (end == std::string::npos)
                break;
              start = end + 1;
            }
            return intercom.CreateRoom(p["name"].value<std::string>(), ids);
          });
  AddTool(
      "self.intercom.join_room",
      "通过六位数字加入码加入或切换到已有对讲房间。用户说‘加入房间’‘切换对讲群’"
      "‘切到频道123456’‘加入码是123456’时调用。加入成功后本机只在新房间收发对讲"
      "音频。"
      "code "
      "为六位数字字符串，必须保留前导零。只给房间名称、未给加入码时先询问加入码"
      "，"
      "不能把房间名称或设备ID当作加入码。受限房间需要邀请权限。"
      "当前返回true仅表示加入请求已发送，不代表已切换成功；false表示请求未发出"
      "。",
      PropertyList({Property("code", kPropertyTypeString)}),
      [&intercom](const PropertyList &p) -> ReturnValue {
        return intercom.JoinRoom(p["code"].value<std::string>());
      });
  AddTool("self.intercom.leave_room",
          "退出本机当前普通对讲房间并返回默认房间。用户说‘退出房间’‘离开这个群’"
          "‘返回默认房间’‘切回默认频道’时调用，不需要加入码。"
          "此操作不会关闭房间、删除房间或断开对讲服务连接，也不影响其他设备。"
          "当前返回true仅表示退出请求已发送，不代表服务端已确认；false表示请求"
          "未发出。",
          PropertyList(), [&intercom](const PropertyList &) -> ReturnValue {
            return intercom.LeaveRoom();
          });
  AddTool("self.intercom.accept_room_invite",
          "接受其他设备发来的对讲房间邀请，申请切换到邀请中的房间。"
          "用户明确说‘接受房间邀请’‘同意加入邀请的群’‘接受加入码123456的邀请’时"
          "调用。"
          "code 为邀请通知里的六位数字加入码字符串，保留前导零。"
          "若不知道邀请的加入码，先询问用户，不要编造；用户未明确同意时不要自动"
          "接受。"
          "当前返回true仅表示接受请求已发送，不代表服务端已确认加入；false表示"
          "请求未发出。",
          PropertyList({Property("code", kPropertyTypeString)}),
          [&intercom](const PropertyList &p) -> ReturnValue {
            return intercom.AcceptInvite(p["code"].value<std::string>());
          });
  // System tools
  AddUserOnlyTool("self.get_system_info", "Get the system information",
                  PropertyList(),
                  [this](const PropertyList &properties) -> ReturnValue {
                    auto &board = Board::GetInstance();
                    return board.GetSystemInfoJson();
                  });

  AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                  [this](const PropertyList &properties) -> ReturnValue {
                    auto &app = Application::GetInstance();
                    app.Schedule([&app]() {
                      ESP_LOGW(TAG, "User requested reboot");
                      vTaskDelay(pdMS_TO_TICKS(1000));

                      app.Reboot();
                    });
                    return true;
                  });

  // Firmware upgrade
  AddUserOnlyTool(
      "self.upgrade_firmware",
      "Upgrade firmware from a specific URL. This will download and install "
      "the firmware, then "
      "reboot the device.",
      PropertyList({Property(
          "url", kPropertyTypeString,
          "The URL of the firmware binary file to download and install")}),
      [this](const PropertyList &properties) -> ReturnValue {
        auto url = properties["url"].value<std::string>();
        ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s",
                 url.c_str());

        auto &app = Application::GetInstance();
        app.Schedule([url, &app]() {
          bool success = app.UpgradeFirmware(url);
          if (!success) {
            ESP_LOGE(TAG, "Firmware upgrade failed");
          }
        });

        return true;
      });

  // Display control
#ifdef HAVE_LVGL
  auto display = Board::GetInstance().GetDisplay();
  if (display != nullptr && display->SupportsGuiOperations()) {
    AddUserOnlyTool(
        "self.screen.get_info",
        "Information about the screen, including width, height, etc.",
        PropertyList(),
        [display](const PropertyList &properties) -> ReturnValue {
          cJSON *json = cJSON_CreateObject();
          cJSON_AddNumberToObject(json, "width", display->width());
          cJSON_AddNumberToObject(json, "height", display->height());
          cJSON_AddBoolToObject(json, "monochrome", display->IsMonochrome());
          return json;
        });

#if CONFIG_LV_USE_SNAPSHOT
    AddUserOnlyTool(
        "self.screen.snapshot",
        "Snapshot the screen and upload it to a specific URL",
        PropertyList({Property("url", kPropertyTypeString),
                      Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
        [display](const PropertyList &properties) -> ToolResult {
          auto url = properties["url"].value<std::string>();
          auto quality = properties["quality"].value<int>();

          std::string jpeg_data;
          if (!display->SnapshotToJpeg(jpeg_data, quality)) {
            return std::unexpected("Failed to snapshot screen");
          }

          ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(),
                   url.c_str());

          // 构造multipart/form-data请求体
          std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

          auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
          http->SetHeader("Content-Type",
                          "multipart/form-data; boundary=" + boundary);
          if (auto opened = http->Open("POST", url); !opened) {
            return std::unexpected("Failed to open URL: " + url + " (" +
                                   opened.error().ToString() + ")");
          }
          auto write_or_fail =
              [&http](const char *data,
                      size_t size) -> std::optional<std::string> {
            if (auto written = http->Write(data, size); !written) {
              return written.error().ToString();
            }
            return std::nullopt;
          };
          {
            // 文件字段头部
            std::string file_header;
            file_header += "--" + boundary + "\r\n";
            file_header += "Content-Disposition: form-data; name=\"file\"; "
                           "filename=\"screenshot.jpg\"\r\n";
            file_header += "Content-Type: image/jpeg\r\n";
            file_header += "\r\n";
            if (auto error =
                    write_or_fail(file_header.c_str(), file_header.size());
                error) {
              http->Close();
              return std::unexpected("Failed to upload screenshot: " + *error);
            }
          }

          // JPEG数据
          if (auto error = write_or_fail((const char *)jpeg_data.data(),
                                         jpeg_data.size());
              error) {
            http->Close();
            return std::unexpected("Failed to upload screenshot: " + *error);
          }

          {
            // multipart尾部
            std::string multipart_footer;
            multipart_footer += "\r\n--" + boundary + "--\r\n";
            if (auto error = write_or_fail(multipart_footer.c_str(),
                                           multipart_footer.size());
                error) {
              http->Close();
              return std::unexpected("Failed to upload screenshot: " + *error);
            }
          }
          if (auto error = write_or_fail("", 0); error) {
            http->Close();
            return std::unexpected("Failed to upload screenshot: " + *error);
          }

          auto upload_status = http->GetStatusCode();
          if (!upload_status) {
            return std::unexpected(upload_status.error().ToString());
          }
          if (*upload_status != 200) {
            return std::unexpected("Unexpected status code: " +
                                   std::to_string(*upload_status));
          }
          std::string result = http->ReadAll();
          http->Close();
          ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
          return true;
        });

    AddUserOnlyTool(
        "self.screen.preview_image", "Preview an image on the screen",
        PropertyList({Property("url", kPropertyTypeString)}),
        [display](const PropertyList &properties) -> ToolResult {
          auto url = properties["url"].value<std::string>();
          auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

          if (auto opened = http->Open("GET", url); !opened) {
            return std::unexpected("Failed to open URL: " + url + " (" +
                                   opened.error().ToString() + ")");
          }
          auto status_code = http->GetStatusCode();
          if (!status_code) {
            return std::unexpected(status_code.error().ToString());
          }
          if (*status_code != 200) {
            return std::unexpected("Unexpected status code: " +
                                   std::to_string(*status_code));
          }

          size_t content_length = http->GetBodyLength();
          using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;
          BufferPtr data(static_cast<char *>(
                             heap_caps_malloc(content_length, MALLOC_CAP_8BIT)),
                         heap_caps_free);
          if (data == nullptr) {
            return std::unexpected("Failed to allocate memory for image: " +
                                   url);
          }
          size_t total_read = 0;
          while (total_read < content_length) {
            auto ret = http->Read(data.get() + total_read,
                                  content_length - total_read);
            if (!ret) {
              return std::unexpected("Failed to download image: " + url);
            }
            if (*ret == 0) {
              break;
            }
            total_read += *ret;
          }
          http->Close();

          auto image =
              std::make_unique<LvglAllocatedImage>(data.release(), total_read);
          if (!image->IsValid()) {
            return std::unexpected("Downloaded image is invalid: " + url);
          }
          display->SetPreviewImage(std::move(image));
          return true;
        });
#endif // CONFIG_LV_USE_SNAPSHOT
  }
#endif // HAVE_LVGL

  // Assets download url (always registered — Settings storage works regardless
  // of partition layout)
  AddUserOnlyTool("self.assets.set_download_url",
                  "Set the download url for the assets",
                  PropertyList({Property("url", kPropertyTypeString)}),
                  [](const PropertyList &properties) -> ReturnValue {
                    auto url = properties["url"].value<std::string>();
                    Settings settings("assets", true);
                    settings.SetString("download_url", url);
                    return true;
                  });
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_Parse(message.c_str()));
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
            "\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message, response_sender);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools, response_sender);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, -32602, "Missing params", response_sender);
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, -32602, "Missing tool name", response_sender);
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, -32602, "Invalid arguments: expected object", response_sender);
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments,
                   std::move(response_sender));
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, -32601, "Method not implemented: " + method_str, response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(int id, const std::string& result,
                            const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message,
                           const ResponseSender& response_sender) {
    CJsonUniquePtr json_message(cJSON_CreateString(message.c_str()));
    if (json_message == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MCP error message");
        return;
    }
    CJsonStringUniquePtr encoded_message(cJSON_PrintUnformatted(json_message.get()));
    if (encoded_message == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize MCP error message");
        return;
    }

    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":";
    payload += encoded_message.get();
    payload += "}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(id, -32603, message, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools,
                             const ResponseSender& response_sender) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit",
                   response_sender);
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json, response_sender);
}

void McpServer::DoToolCall(int id, const std::string &tool_name,
                           const cJSON *tool_arguments,
                           ResponseSender response_sender) {
  auto tool_iter = std::find_if(
      tools_.begin(), tools_.end(),
      [&tool_name](const auto &tool) { return tool->name() == tool_name; });

  if (tool_iter == tools_.end()) {
    ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
    ReplyError(id, -32602, "Unknown tool: " + tool_name, response_sender);
    return;
  }

  McpTool *tool = tool_iter->get();
  PropertyList arguments = tool->properties();
  for (auto &argument : arguments) {
    bool found = false;
    std::expected<void, std::string> validation;
    if (cJSON_IsObject(tool_arguments)) {
      auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
      if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
        validation = argument.set_value<bool>(value->valueint == 1);
        found = true;
      } else if (argument.type() == kPropertyTypeInteger &&
                 cJSON_IsNumber(value)) {
        validation = argument.set_value<int>(value->valueint);
        found = true;
      } else if (argument.type() == kPropertyTypeString &&
                 cJSON_IsString(value)) {
        validation = argument.set_value<std::string>(value->valuestring);
        found = true;
      } else if (value != nullptr) {
        ESP_LOGE(TAG, "tools/call: Invalid type for argument: %s",
                 argument.name().c_str());
        ReplyError(id, -32602, "Invalid type for argument: " + argument.name(),
                   response_sender);
        return;
      }
    }

    if (found && !validation) {
      ESP_LOGE(TAG, "tools/call: %s", validation.error().c_str());
      ReplyError(id, -32602, validation.error(), response_sender);
      return;
    }

    if (!argument.has_default_value() && !found) {
      ESP_LOGE(TAG, "tools/call: Missing required argument: %s",
               argument.name().c_str());
      ReplyError(id, -32602, "Missing required argument: " + argument.name(),
                 response_sender);
      return;
    }
  }

  // Use main thread to call the tool
  auto &app = Application::GetInstance();
  auto invoke = [this, id, tool, arguments = std::move(arguments),
                 response_sender]() {
    auto result = tool->Call(arguments);
    if (!result) {
      ESP_LOGE(TAG, "tools/call: %s", result.error().c_str());
      ReplyError(id, result.error(), response_sender);
      return;
    }
    ReplyResult(id, *result, response_sender);
  };
  if (tool_name == "self.intercom.list_rooms") {
    // This tool waits for a network response; never block Application.
    auto work = new std::function<void()>(std::move(invoke));
    if (xTaskCreate(
            [](void *context) {
              auto task = static_cast<std::function<void()> *>(context);
              (*task)();
              delete task;
              vTaskDelete(nullptr);
            },
            "intercom_rooms", 8192, work, 2, nullptr) != pdPASS) {
      delete work;
      ReplyError(id, "Cannot start room query task", response_sender);
    }
  } else {
    app.Schedule(std::move(invoke));
  }
}
