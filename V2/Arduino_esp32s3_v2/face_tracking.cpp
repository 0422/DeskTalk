// 2026-09-11: Add on-device two-stage face detection and S8 face recognition so only the enrolled owner controls the pan/tilt head.
#include "face_tracking.h"

#include <esp_camera.h>
#include <esp_partition.h>
#include <list>
#include <vector>

#include "camera_config.h"
#include "common.h"
#include "head.h"
#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"
#include "face_recognition_112_v1_s8.hpp"

// 2026-09-11: Keep recognition conservative and process at a low idle rate so WakeNet and networking remain responsive.
static const char *FACE_PARTITION_LABEL = "fr";
static const char *OWNER_FACE_NAME = "owner";
static const float OWNER_SIMILARITY_THRESHOLD = 0.60F;
static const uint8_t OWNER_ENROLLMENT_SAMPLES = 3;
static const unsigned long OWNER_ENROLLMENT_TIMEOUT_MS = 15000UL;
static const unsigned long FACE_FRAME_INTERVAL_MS = 500UL;
static const int FACE_CENTER_DEAD_ZONE_X = 28;
static const int FACE_CENTER_DEAD_ZONE_Y = 24;
static const int FACE_TRACK_STEP_X = 3;
static const int FACE_TRACK_STEP_Y = 2;

// 2026-09-11: Hold camera and owner state in this module so command callbacks cannot manipulate model internals directly.
static bool camera_ready = false;
static bool face_recognition_ready = false;
static bool face_tracking_paused = false;
static bool face_follow_enabled = false;
static bool owner_enrollment_requested = false;
static bool owner_enrollment_started = false;
static uint8_t owner_enrollment_sample_count = 0;
static unsigned long owner_enrollment_request_time = 0;
static unsigned long last_face_frame_time = 0;
static std::vector<int> owner_face_ids;

// 2026-09-11: Use the Arduino Core 3.0.0 S8 recognizer to limit flash and runtime cost compared with the S16 model.
static FaceRecognition112V1S8 face_recognizer;

// 2026-09-11: Treat every flash identity named "owner" as an enrollment sample for the same person.
static void load_owner_face_ids() {
  owner_face_ids.clear();
  std::vector<face_info_t> owners = face_recognizer.get_enrolled_ids_with_name(OWNER_FACE_NAME);
  for (const face_info_t &owner : owners) {
    owner_face_ids.push_back(owner.id);
  }
}

// 2026-09-11: Match only IDs created by owner enrollment; unrelated identities never move the head.
static bool is_owner_face_id(int face_id) {
  for (int owner_id : owner_face_ids) {
    if (owner_id == face_id) {
      return true;
    }
  }
  return false;
}

// 2026-09-11: Move in small dead-zone-controlled steps to reduce servo hunting and vibration.
static void follow_owner_face(const dl::detect::result_t &face, size_t frame_width,
                              size_t frame_height) {
  int face_center_x = (face.box[0] + face.box[2]) / 2;
  int face_center_y = (face.box[1] + face.box[3]) / 2;
  int frame_center_x = frame_width / 2;
  int frame_center_y = frame_height / 2;
  int x_offset = 0;
  int y_offset = 0;

  if (face_center_x < frame_center_x - FACE_CENTER_DEAD_ZONE_X) {
    x_offset = -FACE_TRACK_STEP_X;
  } else if (face_center_x > frame_center_x + FACE_CENTER_DEAD_ZONE_X) {
    x_offset = FACE_TRACK_STEP_X;
  }

  if (face_center_y < frame_center_y - FACE_CENTER_DEAD_ZONE_Y) {
    y_offset = -FACE_TRACK_STEP_Y;
  } else if (face_center_y > frame_center_y + FACE_CENTER_DEAD_ZONE_Y) {
    y_offset = FACE_TRACK_STEP_Y;
  }

#if CAMERA_TRACK_INVERT_X
  x_offset = -x_offset;
#endif
#if CAMERA_TRACK_INVERT_Y
  y_offset = -y_offset;
#endif

  if ((x_offset != 0 || y_offset != 0) && enable_act) {
    head_move(x_offset, y_offset, 3);
    last_time = millis();
  }
}

// 2026-09-11: Capture three owner samples under one name to improve recognition across small pose changes.
static void enroll_owner_from_frame(camera_fb_t *frame,
                                    std::list<dl::detect::result_t> &faces) {
  if (faces.size() != 1) {
    log_warn("Owner enrollment requires exactly one visible face; detected %u",
             (unsigned int)faces.size());
    return;
  }

  if (!owner_enrollment_started) {
    face_recognizer.clear_id(true);
    owner_face_ids.clear();
    owner_enrollment_started = true;
    owner_enrollment_sample_count = 0;
    log_info("Previous owner face data cleared; capturing new owner samples");
  }

  std::vector<int> frame_shape = {
      (int)frame->height, (int)frame->width, 3};
  std::vector<int> &landmarks = faces.front().keypoint;
  int enrolled_id = face_recognizer.enroll_id(
      (uint16_t *)frame->buf, frame_shape, landmarks, OWNER_FACE_NAME, true);

  if (enrolled_id < 0) {
    log_error("Owner face enrollment sample failed");
    return;
  }

  owner_face_ids.push_back(enrolled_id);
  owner_enrollment_sample_count++;
  log_info("Owner face sample %u/%u enrolled as ID %d",
           owner_enrollment_sample_count, OWNER_ENROLLMENT_SAMPLES, enrolled_id);

  if (owner_enrollment_sample_count >= OWNER_ENROLLMENT_SAMPLES) {
    owner_enrollment_requested = false;
    owner_enrollment_started = false;
    face_follow_enabled = true;
    log_info("Owner enrollment complete; face following enabled");
  }
}

// 2026-09-11: Initialize an RGB565 240x240 DVP stream that works with GC2145 and other sensors auto-detected by esp32-camera.
bool setup_face_tracking() {
  // 2026-09-11: Do not enter the camera driver when no DVP module is connected.
#if !DESK_EMOJI_ENABLE_CAMERA
  log_info("Camera disabled; skipping face tracking initialization.");
  return false;
#endif
  if (!psramFound()) {
    log_error("Face tracking requires enabled PSRAM");
    return false;
  }

  camera_config_t config = {};
  config.pin_pwdn = CAMERA_PIN_PWDN;
  config.pin_reset = CAMERA_PIN_RESET;
  config.pin_xclk = CAMERA_PIN_XCLK;
  config.pin_sccb_sda = CAMERA_PIN_SIOD;
  config.pin_sccb_scl = CAMERA_PIN_SIOC;
  config.pin_d7 = CAMERA_PIN_D7;
  config.pin_d6 = CAMERA_PIN_D6;
  config.pin_d5 = CAMERA_PIN_D5;
  config.pin_d4 = CAMERA_PIN_D4;
  config.pin_d3 = CAMERA_PIN_D3;
  config.pin_d2 = CAMERA_PIN_D2;
  config.pin_d1 = CAMERA_PIN_D1;
  config.pin_d0 = CAMERA_PIN_D0;
  config.pin_vsync = CAMERA_PIN_VSYNC;
  config.pin_href = CAMERA_PIN_HREF;
  config.pin_pclk = CAMERA_PIN_PCLK;
  config.xclk_freq_hz = CAMERA_XCLK_FREQUENCY_HZ;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_240X240;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t camera_result = esp_camera_init(&config);
  if (camera_result != ESP_OK) {
    log_error("Camera initialization failed: 0x%X (%s)", camera_result,
              esp_err_to_name(camera_result));
    return false;
  }
  camera_ready = true;

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    log_error("Camera initialized without a sensor descriptor");
    esp_camera_deinit();
    camera_ready = false;
    return false;
  }

  sensor->set_vflip(sensor, CAMERA_SENSOR_VFLIP);
  sensor->set_hmirror(sensor, CAMERA_SENSOR_HMIRROR);
  log_info("Camera detected with PID 0x%04X", sensor->id.PID);
  if (sensor->id.PID != GC2145_PID) {
    log_warn("Expected GC2145 PID 0x%04X; continuing with the auto-detected DVP sensor",
             GC2145_PID);
  }

  face_recognizer.set_thresh(OWNER_SIMILARITY_THRESHOLD);
  if (!face_recognizer.set_partition(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY,
                                     FACE_PARTITION_LABEL)) {
    log_error("Face partition '%s' is unavailable", FACE_PARTITION_LABEL);
    return false;
  }

  int loaded_count = face_recognizer.set_ids_from_flash();
  load_owner_face_ids();
  face_recognition_ready = true;
  face_follow_enabled = !owner_face_ids.empty();
  log_info("Face recognition is Ready: %d identities loaded, %u owner samples",
           loaded_count, (unsigned int)owner_face_ids.size());
  return true;
}

// 2026-09-11: Run two-stage detection and owner recognition only while enrollment or following is active.
void handle_face_tracking() {
  if (!camera_ready || !face_recognition_ready || face_tracking_paused ||
      start_chat || (!face_follow_enabled && !owner_enrollment_requested)) {
    return;
  }

  if (owner_enrollment_requested &&
      millis() - owner_enrollment_request_time >= OWNER_ENROLLMENT_TIMEOUT_MS) {
    owner_enrollment_requested = false;
    // 2026-09-11: Reject and erase a partial replacement enrollment instead of treating fewer than three samples as a valid owner.
    if (owner_enrollment_started) {
      face_recognizer.clear_id(true);
      owner_face_ids.clear();
      face_follow_enabled = false;
    } else {
      load_owner_face_ids();
      face_follow_enabled = !owner_face_ids.empty();
    }
    owner_enrollment_started = false;
    owner_enrollment_sample_count = 0;
    log_warn("Owner enrollment timed out");
    return;
  }

  if (millis() - last_face_frame_time < FACE_FRAME_INTERVAL_MS) {
    return;
  }

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    // 2026-09-11: Back off after a failed capture instead of retrying continuously.
    last_face_frame_time = millis();
    log_error("Camera frame capture failed");
    return;
  }

  if (frame->format != PIXFORMAT_RGB565) {
    log_error("Unsupported face frame format: %d", frame->format);
    esp_camera_fb_return(frame);
    // 2026-09-11: Apply the same idle interval after a rejected frame.
    last_face_frame_time = millis();
    return;
  }

  // 2026-09-11: Construct detectors lazily after camera startup to avoid allocating inference state when no owner feature is active.
  static HumanFaceDetectMSR01 stage_one(0.1F, 0.5F, 10, 0.2F);
  static HumanFaceDetectMNP01 stage_two(0.5F, 0.3F, 5);
  std::vector<int> frame_shape = {
      (int)frame->height, (int)frame->width, 3};
  std::list<dl::detect::result_t> &candidates =
      stage_one.infer((uint16_t *)frame->buf, frame_shape);
  std::list<dl::detect::result_t> &faces =
      stage_two.infer((uint16_t *)frame->buf, frame_shape, candidates);

  if (owner_enrollment_requested) {
    enroll_owner_from_frame(frame, faces);
    esp_camera_fb_return(frame);
    // 2026-09-11: Measure the enrollment interval from completed inference so CPU time is available to WakeNet.
    last_face_frame_time = millis();
    return;
  }

  for (dl::detect::result_t &face : faces) {
    face_info_t recognition = face_recognizer.recognize(
        (uint16_t *)frame->buf, frame_shape, face.keypoint);
    if (recognition.id >= 0 &&
        recognition.similarity >= OWNER_SIMILARITY_THRESHOLD &&
        is_owner_face_id(recognition.id)) {
      follow_owner_face(face, frame->width, frame->height);
      break;
    }
  }

  esp_camera_fb_return(frame);
  // 2026-09-11: Measure the follow interval from completed inference so long model runs cannot trigger back-to-back frames.
  last_face_frame_time = millis();
}

// 2026-09-11: Pause camera inference while ASR, LLM, TTS, and conversation actions use CPU, memory, and the head.
void pause_face_tracking() {
  face_tracking_paused = true;
}

// 2026-09-11: Resume the previously configured idle face-follow behavior after a conversation ends.
void resume_face_tracking() {
  face_tracking_paused = false;
  last_face_frame_time = millis();
}

// 2026-09-11: Arm a short asynchronous enrollment window so serial command handling does not block the main loop.
void request_owner_enrollment() {
  if (!camera_ready || !face_recognition_ready) {
    log_error("Owner enrollment unavailable because camera recognition is not ready");
    return;
  }

  owner_enrollment_requested = true;
  owner_enrollment_started = false;
  owner_enrollment_sample_count = 0;
  owner_enrollment_request_time = millis();
  face_follow_enabled = false;
  log_info("Owner enrollment armed for 15 seconds; show exactly one face");
}

// 2026-09-11: Erase all owner embeddings from both RAM and the dedicated face partition.
void clear_owner_face() {
  if (!face_recognition_ready) {
    log_error("Cannot clear owner because face recognition is not ready");
    return;
  }

  face_recognizer.clear_id(true);
  owner_face_ids.clear();
  owner_enrollment_requested = false;
  owner_enrollment_started = false;
  owner_enrollment_sample_count = 0;
  face_follow_enabled = false;
  log_info("Owner face data cleared");
}

// 2026-09-11: Require stored owner samples before enabling servo following.
void set_face_follow_enabled(bool enabled) {
  if (enabled && (!face_recognition_ready || owner_face_ids.empty())) {
    log_warn("Face following needs an enrolled owner");
    return;
  }

  face_follow_enabled = enabled;
  log_info("Face following %s", enabled ? "enabled" : "disabled");
}

// 2026-09-11: Provide a serial diagnostic without exposing image data or cloud credentials.
void print_face_tracking_status() {
  log_info("Face status: camera=%s, recognition=%s, follow=%s, paused=%s, owner_samples=%u",
           camera_ready ? "ready" : "not-ready",
           face_recognition_ready ? "ready" : "not-ready",
           face_follow_enabled ? "on" : "off",
           face_tracking_paused ? "yes" : "no",
           (unsigned int)owner_face_ids.size());
}

// 2026-09-11: Prevent idle random motion from fighting enrollment or active face following for the same servos.
bool face_tracking_owns_head() {
  return camera_ready && !face_tracking_paused &&
         (face_follow_enabled || owner_enrollment_requested);
}
