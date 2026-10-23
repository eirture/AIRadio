#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <condition_variable>
#include <vector>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>
#include "protocol.h"

class Application;

class Intercom {
public:
  explicit Intercom(Application &app);
  void Start();
  void NetworkChanged(bool connected);
  void Press();
  void Cancel();
  void Tick();
  void PlaybackDrained();
  bool Busy() const;
  bool IsRecording() const;
  const char *Status() const;
  bool CanRecord() const;

private:
  enum class State {
    Disabled,
    Disconnected,
    Connecting,
    Ready,
    Recording,
    Sending,
    Playing
  };
  Application &app_;
  std::atomic<State> state_{State::Disabled};
  std::atomic<bool> network_{false};
  std::atomic<bool> stop_recording_{false};
  std::atomic<bool> cancel_recording_{false};
  std::atomic<bool> session_failed_{false};
  std::atomic<bool> record_task_active_{false};
  std::string id_;
  std::string url_;
  std::string message_id_;
  std::string incoming_id_;
  std::shared_ptr<WebSocket> ws_;
  uint16_t frame_lengths_[500] = {};
  int frame_count_ = 0;
  size_t bytes_ = 0;
  int incoming_frames_ = 0;
  std::atomic<int64_t> playback_started_at_{0};
  std::atomic<int64_t> playback_ended_at_{0};
  std::atomic<bool> hello_ok_{false};
  std::atomic<bool> start_ok_{false};
  std::atomic<bool> end_ok_{false};
  std::atomic<bool> end_pending_{false};
  std::atomic<bool> start_sent_{false};
  std::mutex control_mutex_;
  std::deque<std::string> pending_control_;
  std::mutex frame_mutex_;
  std::deque<std::vector<uint8_t>> outgoing_frames_;
  std::deque<std::unique_ptr<AudioStreamPacket>> incoming_buffer_;
  std::mutex incoming_mutex_;
  int incoming_buffer_target_ = 8;
  bool incoming_playback_started_ = false;
  std::atomic<bool> incoming_initialized_{false};
  int64_t record_started_ = 0;
  void SetState(State state);
  void NetworkTask();
  void RecordTask();
  void Upload();
  void SendRealtimeFrames();
  void OnData(const char *data, size_t len, bool binary);
  bool SendText(const std::string &text);
  void QueueText(std::string text);
  void FlushControl();
  void ResumeAssistantAudio();
};
