#include "intercom.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kMaxBytes = 512 * 1024;
constexpr int kMaxFrames = 500;
constexpr size_t kMaxFrameBytes = 4096;
constexpr size_t kMinFreePsram = 1024 * 1024;
constexpr int kFrameMs = 60;
constexpr int kSampleRate = 16000;
constexpr int kSamples = kFrameMs * kSampleRate / 1000;
constexpr size_t kRealtimeFrameQueue = 12;
constexpr int kJitterFrames = 8;
constexpr char kTag[] = "Intercom";

void LogConfig(const char *source, const std::string &id,
               const std::string &url) {
  const bool valid_url = url.rfind("ws://", 0) == 0;
  ESP_LOGI(kTag, "%s: device_id_len=%u, server_url_len=%u, ws_scheme=%d",
           source, static_cast<unsigned>(id.size()),
           static_cast<unsigned>(url.size()), valid_url);
  if (!id.empty() && valid_url)
    return;

  nvs_handle_t nvs;
  esp_err_t err = nvs_open("intercom", NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "%s: open intercom NVS failed: %s", source,
             esp_err_to_name(err));
    return;
  }
  size_t length = 0;
  esp_err_t id_err = nvs_get_str(nvs, "device_id", nullptr, &length);
  length = 0;
  esp_err_t url_err = nvs_get_str(nvs, "server_url", nullptr, &length);
  ESP_LOGW(kTag, "%s: NVS keys device_id=%s, server_url=%s", source,
           esp_err_to_name(id_err), esp_err_to_name(url_err));
  nvs_close(nvs);
}
} // namespace

Intercom::Intercom(Application &app) : app_(app) {
  Settings settings("intercom");
  id_ = settings.GetString("device_id");
  url_ = settings.GetString("server_url");
  LogConfig("startup", id_, url_);
  if (!id_.empty() && url_.rfind("ws://", 0) == 0)
    state_ = State::Disconnected;
}

void Intercom::Start() {
  xTaskCreate(
      [](void *ctx) {
        static_cast<Intercom *>(ctx)->NetworkTask();
        vTaskDelete(nullptr);
      },
      "intercom_net", 8192, this, 3, nullptr);
}

void Intercom::NetworkChanged(bool connected) {
  if (connected &&
      (state_ == State::Disabled || state_ == State::Disconnected)) {
    Settings settings("intercom");
    id_ = settings.GetString("device_id");
    url_ = settings.GetString("server_url");
    LogConfig("network connected", id_, url_);
    SetState(!id_.empty() && url_.rfind("ws://", 0) == 0 ? State::Disconnected
                                                         : State::Disabled);
  }
  network_ = connected;
  if (!connected) {
    bool was_playing = state_ == State::Playing;
    stop_recording_ = true;
    cancel_recording_ = true;
    session_failed_ = true;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      outgoing_frames_.clear();
    }
    {
      std::lock_guard<std::mutex> lock(incoming_mutex_);
      incoming_buffer_.clear();
      incoming_playback_started_ = false;
      incoming_initialized_ = false;
    }
    SetState(State::Disconnected);
    if (was_playing) {
      incoming_id_.clear();
      playback_started_at_ = 0;
      playback_ended_at_ = 0;
      app_.GetAudioService().ResetDecoder();
      ResumeAssistantAudio();
    }
  }
}

bool Intercom::Busy() const {
  auto state = state_.load();
  return state == State::Recording || state == State::Sending ||
         state == State::Playing;
}

bool Intercom::IsRecording() const { return state_ == State::Recording; }

bool Intercom::CanRecord() const {
  return state_ == State::Ready && app_.GetDeviceState() == kDeviceStateIdle &&
         app_.GetAudioService().IsPlaybackIdle();
}

const char *Intercom::Status() const {
  switch (state_.load()) {
  case State::Disabled:
    return "对讲未配置";
  case State::Disconnected:
    return "对讲未连接";
  case State::Connecting:
    return "对讲连接中";
  case State::Ready:
    if (record_task_active_)
      return "录音准备中";
    if (app_.GetDeviceState() != kDeviceStateIdle)
      return "助手忙";
    if (!app_.GetAudioService().IsPlaybackIdle())
      return "音频播放中";
    return "开始对讲";
  case State::Recording:
    return "说话中";
  case State::Sending:
    return "发送中";
  case State::Playing:
    return "说话中";
  }
  return "对讲不可用";
}

void Intercom::SetState(State state) { state_ = state; }

std::string Intercom::ListRooms() {
  std::unique_lock<std::mutex> lock(rooms_mutex_);
  if (!rooms_request_id_.empty())
    return "{\"ok\":false,\"error\":\"room query busy\"}";
  if (state_ == State::Disabled || state_ == State::Disconnected ||
      state_ == State::Connecting)
    return "{\"ok\":false,\"error\":\"intercom disconnected\"}";
  rooms_request_id_ = std::to_string(esp_random());
  rooms_response_.clear();
  QueueText("{\"type\":\"list_rooms\",\"request_id\":\"" + rooms_request_id_ +
            "\"}");
  bool received = rooms_cv_.wait_for(lock, std::chrono::seconds(5), [this]() {
    return !rooms_response_.empty();
  });
  rooms_request_id_.clear();
  return received ? rooms_response_
                  : "{\"ok\":false,\"error\":\"room query timed out\"}";
}

bool Intercom::CreateRoom(const std::string &name,
                          const std::vector<std::string> &invite_ids) {
  if (!ws_ || !ws_->IsConnected() || name.empty())
    return false;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "create_room");
  cJSON_AddStringToObject(root, "name", name.c_str());
  cJSON_AddStringToObject(root, "creator_device_id", id_.c_str());
  cJSON *ids = cJSON_AddArrayToObject(root, "invite_device_ids");
  for (const auto &id : invite_ids)
    cJSON_AddItemToArray(ids, cJSON_CreateString(id.c_str()));
  char *raw = cJSON_PrintUnformatted(root);
  bool ok = SendText(raw);
  cJSON_free(raw);
  cJSON_Delete(root);
  return ok;
}
bool Intercom::JoinRoom(const std::string &code) {
  return SendText("{\"type\":\"join_room\",\"code\":\"" + code + "\"}");
}
bool Intercom::LeaveRoom() { return SendText("{\"type\":\"leave_room\"}"); }
bool Intercom::AcceptInvite(const std::string &code) {
  return SendText("{\"type\":\"accept_room_invite\",\"code\":\"" + code +
                  "\"}");
}

void Intercom::ResumeAssistantAudio() {
  if (app_.GetDeviceState() == kDeviceStateIdle)
    app_.GetAudioService().EnableWakeWordDetection(true);
}

void Intercom::Press() {
  ESP_LOGI(kTag, "Record button: intercom_state=%d, device_state=%d",
           static_cast<int>(state_.load()),
           static_cast<int>(app_.GetDeviceState()));
  if (state_ == State::Recording) {
    ESP_LOGI(kTag, "Stopping recording: frames=%d", frame_count_);
    stop_recording_ = true;
    SetState(State::Sending);
    return;
  }
  if (!CanRecord() || record_task_active_) {
    ESP_LOGW(kTag, "Recording unavailable: status=%s, task_active=%d", Status(),
             record_task_active_.load());
    return;
  }
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kMinFreePsram) {
    ESP_LOGW(kTag, "Cannot start realtime talk: free PSRAM below reserve");
    app_.Schedule([this]() {
      Board::GetInstance().GetDisplay()->ShowNotification("PSRAM不足");
    });
    return;
  }
  app_.GetAudioService().EnableWakeWordDetection(false);
  stop_recording_ = cancel_recording_ = session_failed_ = false;
  frame_count_ = 0;
  bytes_ = 0;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    outgoing_frames_.clear();
  }
  message_id_ = id_ + "-" + std::to_string(esp_random());
  start_ok_ = false;
  end_ok_ = false;
  end_pending_ = false;
  start_sent_ = SendText("{\"type\":\"start\",\"message_id\":\"" + message_id_ +
                         "\",\"frame_ms\":60,\"sample_rate\":16000}");
  if (!start_sent_) {
    ESP_LOGW(kTag, "Failed to send realtime start message=%s",
             message_id_.c_str());
    ResumeAssistantAudio();
    return;
  }
  ESP_LOGI(kTag, "Realtime talk start sent message=%s", message_id_.c_str());
  record_started_ = esp_timer_get_time();
  if (!app_.GetAudioService().StartIntercomCapture()) {
    ESP_LOGW(kTag, "Audio capture refused to start");
    ResumeAssistantAudio();
    return;
  }
  SetState(State::Recording);
  ESP_LOGI(kTag, "Recording started");
  record_task_active_ = true;
  if (xTaskCreate(
          [](void *ctx) {
            static_cast<Intercom *>(ctx)->RecordTask();
            vTaskDelete(nullptr);
          },
          "intercom_record", 40 * 1024, this, 5, nullptr) != pdPASS) {
    ESP_LOGE(kTag, "Failed to create recording task");
    record_task_active_ = false;
    app_.GetAudioService().StopIntercomCapture();
    SetState(State::Ready);
    ResumeAssistantAudio();
  }
}

void Intercom::Cancel() {
  if (state_ == State::Recording) {
    ESP_LOGI(kTag, "Cancelling realtime talk message=%s frames=%d",
             message_id_.c_str(), frame_count_);
    cancel_recording_ = true;
    stop_recording_ = true;
  }
}

void Intercom::Tick() {
  if (state_ == State::Recording &&
      esp_timer_get_time() - record_started_ >= 30 * 1000000LL) {
    stop_recording_ = true;
    Board::GetInstance().GetDisplay()->ShowNotification(
        "录音已达上限，正在发送");
  }
  if (state_ == State::Playing && incoming_id_.empty() &&
      playback_ended_at_.load() != 0) {
    PlaybackDrained();
  }
  if (state_ == State::Playing && !incoming_id_.empty() &&
      playback_started_at_.load() != 0 &&
      esp_timer_get_time() - playback_started_at_.load() >
          (incoming_frames_ == 0 ? 5 : 40) * 1000000LL) {
    ESP_LOGW(kTag, "Receive timed out: message=%s frames=%d",
             incoming_id_.c_str(), incoming_frames_);
    incoming_id_.clear();
    playback_started_at_ = 0;
    playback_ended_at_ = 0;
    app_.GetAudioService().ResetDecoder();
    SetState(network_ ? State::Ready : State::Disconnected);
    ResumeAssistantAudio();
  }
}

void Intercom::PlaybackDrained() {
  if (state_ == State::Playing && incoming_id_.empty() &&
      playback_ended_at_.load() != 0 &&
      app_.GetAudioService().IsPlaybackIdle()) {
    const auto elapsed_ms =
        (esp_timer_get_time() - playback_ended_at_.load()) / 1000;
    ESP_LOGI(kTag, "Receive playback drained: frames=%d, elapsed_ms=%lld",
             incoming_frames_, static_cast<long long>(elapsed_ms));
    playback_ended_at_ = 0;
    SetState(State::Ready);
    ResumeAssistantAudio();
  }
}

void Intercom::RecordTask() {
  ESP_LOGI(kTag, "Recording task entered: stack_free=%u",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  uint8_t *frame = static_cast<uint8_t *>(
      heap_caps_malloc(kMaxFrameBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!frame) {
    ESP_LOGE(kTag, "Failed to allocate Opus output buffer");
    session_failed_ = true;
  }
  void *encoder = nullptr;
  esp_opus_enc_config_t cfg = AS_OPUS_ENC_CONFIG();
  if (!session_failed_ &&
      (esp_opus_enc_open(&cfg, sizeof(cfg), &encoder) != ESP_AUDIO_ERR_OK ||
       !encoder))
    session_failed_ = true;
  int frame_bytes = 0, out_bytes = 0;
  if (encoder)
    esp_opus_enc_get_frame_size(encoder, &frame_bytes, &out_bytes);
  ESP_LOGI(kTag,
           "Recording encoder: ready=%d, input_bytes=%d, output_bytes=%d, "
           "stack_free=%u",
           encoder != nullptr && !session_failed_, frame_bytes, out_bytes,
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  if (frame_bytes <= 0 || out_bytes <= 0 || out_bytes > kMaxFrameBytes)
    session_failed_ = true;
  std::vector<int16_t> pcm;
  bool first_frame = true;
  while (!stop_recording_ && !session_failed_) {
    if (frame_count_ >= kMaxFrames || bytes_ >= kMaxBytes)
      break;
    if (!app_.GetAudioService().PopIntercomPcm(pcm)) {
      session_failed_ = true;
      break;
    }
    if (first_frame) {
      ESP_LOGI(kTag, "First capture: samples=%u, stack_free=%u",
               static_cast<unsigned>(pcm.size()),
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
      first_frame = false;
    }
    if (pcm.size() == kSamples * 2) {
      for (int i = 0; i < kSamples; ++i)
        pcm[i] = pcm[i * 2];
      pcm.resize(kSamples);
    }
    if (pcm.size() * sizeof(int16_t) != static_cast<size_t>(frame_bytes)) {
      session_failed_ = true;
      break;
    }
    esp_audio_enc_in_frame_t in = {.buffer =
                                       reinterpret_cast<uint8_t *>(pcm.data()),
                                   .len = static_cast<uint32_t>(frame_bytes)};
    esp_audio_enc_out_frame_t out = {.buffer = frame,
                                     .len = static_cast<uint32_t>(out_bytes),
                                     .encoded_bytes = 0};
    if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK ||
        out.encoded_bytes == 0 || out.encoded_bytes > kMaxFrameBytes) {
      session_failed_ = true;
      break;
    }
    if (bytes_ + out.encoded_bytes > kMaxBytes)
      break;
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (outgoing_frames_.size() >= kRealtimeFrameQueue) {
        ESP_LOGW(kTag, "Realtime send queue full message=%s frames=%d",
                 message_id_.c_str(), frame_count_);
        session_failed_ = true;
        break;
      }
      outgoing_frames_.emplace_back(frame, frame + out.encoded_bytes);
    }
    frame_lengths_[frame_count_++] = out.encoded_bytes;
    bytes_ += out.encoded_bytes;
  }
  app_.GetAudioService().StopIntercomCapture();
  if (encoder)
    esp_opus_enc_close(encoder);
  heap_caps_free(frame);
  if (cancel_recording_ || session_failed_ || frame_count_ == 0 || !network_) {
    ESP_LOGW(kTag,
             "Recording ended without upload: cancelled=%d failed=%d "
             "frames=%d network=%d",
             cancel_recording_.load(), session_failed_.load(), frame_count_,
             network_.load());
    if (start_sent_ && network_ && !session_failed_)
      QueueText("{\"type\":\"abort\",\"message_id\":\"" + message_id_ + "\"}");
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      outgoing_frames_.clear();
    }
    if (state_ == State::Recording)
      SetState(network_ && !session_failed_ ? State::Ready
                                            : State::Disconnected);
    ResumeAssistantAudio();
    record_task_active_ = false;
    return;
  }
  end_pending_ = true;
  ESP_LOGI(kTag, "Realtime capture ended message=%s frames=%d bytes=%u",
           message_id_.c_str(), frame_count_, static_cast<unsigned>(bytes_));
  record_task_active_ = false;
}

bool Intercom::SendText(const std::string &text) {
  return ws_ && ws_->IsConnected() && ws_->Send(text);
}

void Intercom::QueueText(std::string text) {
  std::lock_guard<std::mutex> lock(control_mutex_);
  if (pending_control_.size() < 8)
    pending_control_.push_back(std::move(text));
  else
    session_failed_ = true;
}

void Intercom::FlushControl() {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(control_mutex_);
    pending.swap(pending_control_);
  }
  for (auto &message : pending)
    if (!SendText(message)) {
      ESP_LOGW(kTag, "Failed to send control: %s", message.c_str());
      session_failed_ = true;
    } else if (message.find("\"receive_ready\"") != std::string::npos) {
      ESP_LOGI(kTag, "Receive ready sent");
    }
}

void Intercom::Upload() { SendRealtimeFrames(); }

void Intercom::SendRealtimeFrames() {
  std::vector<uint8_t> frame;
  bool queue_empty = false;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!outgoing_frames_.empty()) {
      frame = std::move(outgoing_frames_.front());
      outgoing_frames_.pop_front();
    }
    queue_empty = outgoing_frames_.empty();
  }
  if (!frame.empty() && ws_ && ws_->IsConnected()) {
    if (!ws_->Send(frame.data(), frame.size(), true)) {
      ESP_LOGW(kTag, "Realtime frame send failed message=%s",
               message_id_.c_str());
      session_failed_ = true;
    } else if (frame_count_ == 1) {
      ESP_LOGD(kTag, "Realtime first frame sent message=%s bytes=%u",
               message_id_.c_str(), static_cast<unsigned>(frame.size()));
    }
  }
  if (end_pending_ && queue_empty && !record_task_active_ && !end_ok_) {
    if (SendText("{\"type\":\"end\",\"message_id\":\"" + message_id_ +
                 "\",\"frame_count\":" + std::to_string(frame_count_) + "}")) {
      ESP_LOGI(kTag, "Realtime end sent message=%s frames=%d",
               message_id_.c_str(), frame_count_);
      end_pending_ = false;
      SetState(State::Sending);
    }
  }
  if (state_ == State::Sending && end_ok_) {
    SetState(network_ && ws_ && ws_->IsConnected() ? State::Ready
                                                   : State::Disconnected);
    ResumeAssistantAudio();
  }
}

void Intercom::OnData(const char *data, size_t len, bool binary) {
  if (binary) {
    if (state_ != State::Playing || len == 0 || len > kMaxFrameBytes ||
        incoming_frames_ >= kMaxFrames) {
      ESP_LOGW(kTag, "Dropping audio frame: state=%d len=%u frames=%d",
               static_cast<int>(state_.load()), static_cast<unsigned>(len),
               incoming_frames_);
      return;
    }
    // WebCodecs may emit tiny DTX packets. The ESP Opus decoder used by the
    // audio service does not accept those packets reliably, so discard them
    // before they enter the jitter/decode queues.
    if (len < 8) {
      ESP_LOGW(kTag, "Dropping too-small Opus frame: bytes=%u",
               static_cast<unsigned>(len));
      return;
    }
    if (incoming_frames_ == 0)
      ESP_LOGI(kTag, "First receive frame: bytes=%u",
               static_cast<unsigned>(len));
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = kSampleRate;
    packet->frame_duration = kFrameMs;
    packet->payload.assign(data, data + len);
    std::lock_guard<std::mutex> lock(incoming_mutex_);
    if (incoming_buffer_.size() >= 12) {
      ESP_LOGW(kTag, "Receive jitter buffer full message=%s frames=%d",
               incoming_id_.c_str(), incoming_frames_);
      incoming_id_.clear();
      incoming_buffer_.clear();
      incoming_playback_started_ = false;
      app_.Schedule([this]() { app_.GetAudioService().ResetDecoder(); });
      return;
    }
    incoming_buffer_.push_back(std::move(packet));
    ++incoming_frames_;
    if (incoming_initialized_ &&
        (incoming_playback_started_ ||
         incoming_buffer_.size() >=
             static_cast<size_t>(incoming_buffer_target_))) {
      incoming_playback_started_ = true;
      while (!incoming_buffer_.empty()) {
        if (!app_.GetAudioService().TryPushPacketToDecodeQueue(
                incoming_buffer_.front()))
          break;
        incoming_buffer_.pop_front();
      }
      ESP_LOGD(kTag, "Receive jitter buffer primed message=%s frames=%d",
               incoming_id_.c_str(), incoming_frames_);
    }
    return;
  }
  if (len > 8192)
    return;
  cJSON *json = cJSON_ParseWithLength(data, len);
  if (!json)
    return;
  auto type = cJSON_GetObjectItem(json, "type");
  auto id = cJSON_GetObjectItem(json, "message_id");
  auto ok = cJSON_GetObjectItem(json, "ok");
  auto room_name = cJSON_GetObjectItem(json, "room_name");
  const char *name = cJSON_IsString(type) ? type->valuestring : "";
  const char *mid = cJSON_IsString(id) ? id->valuestring : "";
  if (!strcmp(name, "list_rooms_ack")) {
    auto request = cJSON_GetObjectItem(json, "request_id");
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    if (cJSON_IsString(request) && !rooms_request_id_.empty() &&
        rooms_request_id_ == request->valuestring) {
      rooms_response_.assign(data, len);
      rooms_cv_.notify_all();
    }
    cJSON_Delete(json);
    return;
  }
  if (cJSON_IsString(room_name))
    room_name_ = room_name->valuestring;
  if (!strcmp(name, "hello_ack"))
    hello_ok_ = cJSON_IsTrue(ok);
  else if (!strcmp(name, "start_ack") && message_id_ == mid) {
    start_ok_ = cJSON_IsTrue(ok);
    if (!start_ok_)
      session_failed_ = true;
  } else if (!strcmp(name, "end_ack") && message_id_ == mid)
    end_ok_ = cJSON_IsTrue(ok);
  else if (!strcmp(name, "start")) {
    auto frame_ms = cJSON_GetObjectItem(json, "frame_ms");
    auto rate = cJSON_GetObjectItem(json, "sample_rate");
    if (strlen(mid) <= 64 && cJSON_IsNumber(frame_ms) &&
        frame_ms->valueint == kFrameMs && cJSON_IsNumber(rate) &&
        rate->valueint == kSampleRate && CanRecord()) {
      incoming_id_ = mid;
      incoming_frames_ = 0;
      incoming_buffer_target_ = kJitterFrames;
      incoming_playback_started_ = false;
      incoming_initialized_ = false;
      playback_started_at_ = esp_timer_get_time();
      playback_ended_at_ = 0;
      ESP_LOGI(kTag, "Receive start: message=%s", mid);
      SetState(State::Playing);
      app_.Schedule([this, message = incoming_id_]() {
        app_.GetAudioService().EnableWakeWordDetection(false);
        app_.GetAudioService().ResetDecoder();
        app_.GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
        incoming_initialized_ = true;
        QueueText("{\"type\":\"receive_ready\",\"message_id\":\"" + message +
                  "\"}");
        ESP_LOGI(kTag, "Receive ready queued: message=%s", message.c_str());
      });
    } else {
      ESP_LOGW(kTag, "Receive start rejected: message=%s status=%s", mid,
               Status());
      QueueText("{\"type\":\"busy\",\"message_id\":\"" + std::string(mid) +
                "\"}");
    }
  } else if ((!strcmp(name, "end") || !strcmp(name, "abort")) &&
             incoming_id_ == mid && state_ == State::Playing) {
    auto count = cJSON_GetObjectItem(json, "frame_count");
    ESP_LOGI(kTag, "Receive %s: message=%s expected=%d received=%d", name, mid,
             cJSON_IsNumber(count) ? count->valueint : -1, incoming_frames_);
    {
      std::lock_guard<std::mutex> lock(incoming_mutex_);
      while (!incoming_buffer_.empty()) {
        if (!app_.GetAudioService().TryPushPacketToDecodeQueue(
                incoming_buffer_.front()))
          break;
        incoming_buffer_.pop_front();
      }
      if (!incoming_buffer_.empty()) {
        ESP_LOGW(
            kTag,
            "Dropping %u buffered frames at receive end: decode queue full",
            static_cast<unsigned>(incoming_buffer_.size()));
        incoming_buffer_.clear();
      }
      incoming_playback_started_ = false;
      incoming_initialized_ = false;
    }
    if (strcmp(name, "abort") == 0 || !cJSON_IsNumber(count) ||
        count->valueint != incoming_frames_) {
      app_.Schedule([this]() { app_.GetAudioService().ResetDecoder(); });
    }
    incoming_id_.clear();
    playback_started_at_ = 0;
    playback_ended_at_ = esp_timer_get_time();
    app_.Schedule([this]() { PlaybackDrained(); });
  } else if (!strcmp(name, "error"))
    session_failed_ = true;
  cJSON_Delete(json);
}

void Intercom::NetworkTask() {
  int backoff = 1;
  while (true) {
    if (!network_ || state_ == State::Disabled) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    session_failed_ = false;
    SetState(State::Connecting);
    ESP_LOGI(kTag, "Connecting to intercom server");
    ws_ = Board::GetInstance().GetNetwork()->CreateWebSocket();
    if (ws_) {
      ws_->OnData([this](const char *data, size_t len, bool binary) {
        OnData(data, len, binary);
      });
      ws_->OnDisconnected([this]() { session_failed_ = true; });
      if (ws_->Connect(url_.c_str())) {
        hello_ok_ = false;
        SendText("{\"type\":\"hello\",\"version\":1,\"device_id\":\"" + id_ +
                 "\"}");
        for (int i = 0; i < 40 && !hello_ok_ && ws_->IsConnected(); ++i)
          vTaskDelay(pdMS_TO_TICKS(50));
        if (hello_ok_) {
          backoff = 1;
          SetState(State::Ready);
          ESP_LOGI(kTag, "Intercom server ready");
          while (network_ && ws_->IsConnected() && !session_failed_) {
            FlushControl();
            if ((state_ == State::Recording || state_ == State::Sending) &&
                !record_task_active_)
              Upload();
            else if (state_ == State::Recording || state_ == State::Sending)
              SendRealtimeFrames();
            vTaskDelay(pdMS_TO_TICKS(30));
          }
        }
      }
      ws_->Close();
      ws_.reset();
    }
    ESP_LOGW(kTag, "Intercom connection ended: hello_ok=%d, failed=%d",
             hello_ok_.load(), session_failed_.load());
    if (state_ == State::Playing) {
      incoming_id_.clear();
      playback_started_at_ = 0;
      playback_ended_at_ = 0;
      app_.Schedule([this]() {
        app_.GetAudioService().ResetDecoder();
        ResumeAssistantAudio();
      });
    }
    stop_recording_ = cancel_recording_ = session_failed_ = true;
    SetState(State::Disconnected);
    while (record_task_active_)
      vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
    backoff = std::min(backoff * 2, 30);
  }
}
