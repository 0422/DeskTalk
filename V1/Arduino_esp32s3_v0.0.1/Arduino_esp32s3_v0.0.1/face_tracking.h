// 2026-09-11: Expose GC2145-compatible local face enrollment, owner recognition, and head-follow controls.
#ifndef FaceTracking_h
#define FaceTracking_h

bool setup_face_tracking();
void handle_face_tracking();
void pause_face_tracking();
void resume_face_tracking();
void request_owner_enrollment();
void clear_owner_face();
void set_face_follow_enabled(bool enabled);
void print_face_tracking_status();
bool face_tracking_owns_head();

#endif
