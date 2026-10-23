#include "intercom.h"

#include "application.h"
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
    return "录音";
  case State::Recording:
    return "发送";
  case State::Sending:
    return "发送中";
  case State::Playing:
    return "播放中";
  }
  return "对讲不可用";
}

void Intercom::SetState(State state) { state_ = state; }

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
    return;
  }
  if (!CanRecord() || record_task_active_) {
    ESP_LOGW(kTag, "Recording unavailable: status=%s, task_active=%d", Status(),
             record_task_active_.load());
    return;
  }
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kMinFreePsram + kMaxBytes) {
    app_.Schedule([this]() {
      Board::GetInstance().GetDisplay()->ShowNotification("PSRAM不足");
    });
    return;
  }
  app_.GetAudioService().EnableWakeWordDetection(false);
  stop_recording_ = cancel_recording_ = session_failed_ = false;
  buffer_ = static_cast<uint8_t *>(
      heap_caps_malloc(kMaxBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!buffer_) {
    ResumeAssistantAudio();
    app_.Schedule([this]() {
      Board::GetInstance().GetDisplay()->ShowNotification("PSRAM分配失败");
    });
    return;
  }
  frame_count_ = 0;
  bytes_ = 0;
  record_started_ = esp_timer_get_time();
  if (!app_.GetAudioService().StartIntercomCapture()) {
    ESP_LOGW(kTag, "Audio capture refused to start");
    heap_caps_free(buffer_);
    buffer_ = nullptr;
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
    heap_caps_free(buffer_);
    buffer_ = nullptr;
    SetState(State::Ready);
    ResumeAssistantAudio();
  }
}

void Intercom::Cancel() {
  if (state_ == State::Recording) {
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
    memcpy(buffer_ + bytes_, frame, out.encoded_bytes);
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
    heap_caps_free(buffer_);
    buffer_ = nullptr;
    if (state_ == State::Recording)
      SetState(network_ && !session_failed_ ? State::Ready
                                            : State::Disconnected);
    ResumeAssistantAudio();
    record_task_active_ = false;
    return;
  }
  SetState(State::Sending);
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

void Intercom::Upload() {
  size_t offset = 0;
  message_id_ = id_ + "-" + std::to_string(esp_random());
  start_ok_ = end_ok_ = false;
  if (!SendText("{\"type\":\"start\",\"message_id\":\"" + message_id_ +
                "\",\"frame_ms\":60,\"sample_rate\":16000}"))
    goto done;
  for (int i = 0; i < 80 && !start_ok_ && !session_failed_ && network_; ++i) {
    FlushControl();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!start_ok_)
    goto done;
  for (int i = 0; i < frame_count_; ++i) {
    FlushControl();
    if (!network_ || session_failed_ ||
        !ws_->Send(buffer_ + offset, frame_lengths_[i], true))
      goto done;
    offset += frame_lengths_[i];
    vTaskDelay(pdMS_TO_TICKS(kFrameMs));
  }
  if (!SendText("{\"type\":\"end\",\"message_id\":\"" + message_id_ +
                "\",\"frame_count\":" + std::to_string(frame_count_) + "}"))
    goto done;
  for (int i = 0; i < 40 && !end_ok_ && !session_failed_ && network_; ++i) {
    FlushControl();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
done:
  if (!end_ok_)
    SendText("{\"type\":\"abort\"}");
  heap_caps_free(buffer_);
  buffer_ = nullptr;
  SetState(network_ && ws_ && ws_->IsConnected() ? State::Ready
                                                 : State::Disconnected);
  ResumeAssistantAudio();
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
    if (incoming_frames_ == 0)
      ESP_LOGI(kTag, "First receive frame: bytes=%u",
               static_cast<unsigned>(len));
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = kSampleRate;
    packet->frame_duration = kFrameMs;
    packet->payload.assign(data, data + len);
    if (!app_.GetAudioService().PushPacketToDecodeQueue(std::move(packet))) {
      ESP_LOGW(kTag, "Receive decode queue full: frames=%d", incoming_frames_);
      QueueText("{\"type\":\"busy\",\"message_id\":\"" + incoming_id_ + "\"}");
      app_.Schedule([this]() {
        incoming_id_.clear();
        playback_started_at_ = 0;
        playback_ended_at_ = 0;
        app_.GetAudioService().ResetDecoder();
        SetState(State::Ready);
        ResumeAssistantAudio();
      });
    } else
      ++incoming_frames_;
    return;
  }
  if (len > 1024)
    return;
  cJSON *json = cJSON_ParseWithLength(data, len);
  if (!json)
    return;
  auto type = cJSON_GetObjectItem(json, "type");
  auto id = cJSON_GetObjectItem(json, "message_id");
  auto ok = cJSON_GetObjectItem(json, "ok");
  const char *name = cJSON_IsString(type) ? type->valuestring : "";
  const char *mid = cJSON_IsString(id) ? id->valuestring : "";
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
      playback_started_at_ = esp_timer_get_time();
      playback_ended_at_ = 0;
      ESP_LOGI(kTag, "Receive start: message=%s", mid);
      SetState(State::Playing);
      app_.Schedule([this, message = incoming_id_]() {
        app_.GetAudioService().EnableWakeWordDetection(false);
        app_.GetAudioService().ResetDecoder();
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
            if (state_ == State::Sending && !record_task_active_)
              Upload();
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
