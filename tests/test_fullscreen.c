// Drives the foreign-toplevel callbacks with deterministic window events.
#include "../src/platform/fullscreen.c"
#include "test_helpers.h"

static const char *niri_output;
const char *niri_focused_output(void) {
  return niri_output;
}

// Linked with --wrap: handles are fakes, so destroy requests go nowhere.
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy);
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy,
                                               uint32_t opcode,
                                               const struct wl_interface *type,
                                               uint32_t version, uint32_t flags,
                                               ...);
uint32_t __wrap_wl_proxy_get_version(struct wl_proxy *proxy) {
  (void)proxy;
  return 1;
}
struct wl_proxy *__wrap_wl_proxy_marshal_flags(struct wl_proxy *proxy,
                                               uint32_t opcode,
                                               const struct wl_interface *type,
                                               uint32_t version, uint32_t flags,
                                               ...) {
  (void)proxy;
  (void)opcode;
  (void)type;
  (void)version;
  (void)flags;
  return NULL;
}

// Opaque identities: compared, never passed to Wayland.
static int identities[8];
#define OUTPUT_A ((struct wl_output *)&identities[0])
#define OUTPUT_B ((struct wl_output *)&identities[1])

static toplevel_t *add_window(void) {
  toplevel_t *window = calloc(1, sizeof(*window));
  TEST_ASSERT(window);
  window->handle = (struct zwlr_foreign_toplevel_handle_v1 *)window;
  wl_list_insert(&toplevels, &window->link);
  return window;
}

static void commit(toplevel_t *window, struct wl_output *output, bool activated,
                   bool fullscreen) {
  struct wl_array values;
  wl_array_init(&values);
  if (activated) {
    uint32_t *value = wl_array_add(&values, sizeof(*value));
    TEST_ASSERT(value);
    *value = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED;
  }
  if (fullscreen) {
    uint32_t *value = wl_array_add(&values, sizeof(*value));
    TEST_ASSERT(value);
    *value = ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN;
  }
  state(window, NULL, &values);
  wl_array_release(&values);
  if (output) {
    leave(window, NULL, output == OUTPUT_A ? OUTPUT_B : OUTPUT_A);
    enter(window, NULL, output);
  }
  done(window, NULL);
}

static void reset(void) {
  toplevel_t *window;
  toplevel_t *next;
  wl_list_for_each_safe(window, next, &toplevels, link) {
    wl_list_remove(&window->link);
    free(window);
  }
  activations = forgotten_everywhere = 0;
  memset(forgotten, 0, sizeof(forgotten));
  memset(focus_lost, 0, sizeof(focus_lost));
  focus_lost_unknown = false;
  output_events = false;
  niri_output = NULL;
  fullscreen_take_changed();
}

static void test_multi_monitor(void) {
  reset();
  toplevel_t *video = add_window();
  toplevel_t *terminal = add_window();
  commit(terminal, OUTPUT_B, false, false);
  commit(video, OUTPUT_A, true, true);
  TEST_ASSERT(fullscreen_on(OUTPUT_A) && !fullscreen_on(OUTPUT_B));
  TEST_ASSERT(fullscreen_focused_output(NULL) == OUTPUT_A);

  // Focus moves to another output as one batch: a deactivation and an
  // activation. The video keeps covering its own output.
  commit(video, NULL, false, true);
  commit(terminal, NULL, true, false);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A) && !fullscreen_on(OUTPUT_B));
  TEST_ASSERT(fullscreen_focused_output(NULL) == OUTPUT_B);

  // Leaving fullscreen while unfocused uncovers the output.
  commit(video, NULL, false, false);
  fullscreen_settle();
  TEST_ASSERT(!fullscreen_on(OUTPUT_A));
  commit(video, NULL, false, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));

  // A newer window focused on the same output replaces it.
  toplevel_t *editor = add_window();
  commit(terminal, NULL, false, false);
  commit(editor, OUTPUT_A, true, false);
  fullscreen_settle();
  TEST_ASSERT(!fullscreen_on(OUTPUT_A));

  // Closing it while focus returns elsewhere uncovers the video again.
  closed(editor, NULL);
  commit(terminal, NULL, true, false);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
}

static void test_empty_workspaces(void) {
  reset();
  toplevel_t *video = add_window();
  toplevel_t *terminal = add_window();
  commit(terminal, OUTPUT_B, false, false);
  commit(video, OUTPUT_A, true, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
  // Without niri, losing focus to nothing means an empty workspace took over
  // the output focus left.
  commit(video, NULL, false, true);
  TEST_ASSERT(fullscreen_on(OUTPUT_A));  // Not before the batch ends
  fullscreen_settle();
  TEST_ASSERT(!fullscreen_on(OUTPUT_A));
  commit(video, NULL, true, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
  // An empty workspace on another output leaves this one covered.
  commit(video, NULL, false, true);
  commit(terminal, NULL, true, false);
  fullscreen_settle();
  commit(terminal, NULL, false, false);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));

  // niri names the output with the focused empty workspace instead.
  commit(video, NULL, true, true);
  fullscreen_settle();
  niri_output = "B";
  commit(video, NULL, false, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
  fullscreen_forget(OUTPUT_B);
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
  fullscreen_forget(OUTPUT_A);
  TEST_ASSERT(!fullscreen_on(OUTPUT_A));
  // Focusing the window again counts again.
  commit(video, NULL, true, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A));
}

static void test_changes(void) {
  reset();
  toplevel_t *window = add_window();
  commit(window, OUTPUT_A, true, false);
  TEST_ASSERT(fullscreen_take_changed());
  // Metadata-only updates report no change.
  text_event(window, NULL, "new title");
  done(window, NULL);
  TEST_ASSERT(!fullscreen_take_changed());
  commit(window, NULL, true, false);
  TEST_ASSERT(!fullscreen_take_changed());
  // Pending output changes count only once committed.
  leave(window, NULL, OUTPUT_A);
  enter(window, NULL, OUTPUT_B);
  TEST_ASSERT(fullscreen_focused_output(NULL) == OUTPUT_A);
  done(window, NULL);
  TEST_ASSERT(fullscreen_take_changed());
  TEST_ASSERT(fullscreen_focused_output(NULL) == OUTPUT_B);
  // A window spanning outputs prefers the current one.
  enter(window, NULL, OUTPUT_A);
  done(window, NULL);
  TEST_ASSERT(fullscreen_focused_output(OUTPUT_A) == OUTPUT_A);
  TEST_ASSERT(fullscreen_focused_output(OUTPUT_B) == OUTPUT_B);
  // A removed output is no longer reported.
  fullscreen_output_removed(OUTPUT_B);
  TEST_ASSERT(fullscreen_focused_output(OUTPUT_B) == OUTPUT_A);
}

static void test_without_output_events(void) {
  reset();
  // Compositors without output events place windows on every output.
  toplevel_t *window = add_window();
  commit(window, NULL, true, true);
  fullscreen_settle();
  TEST_ASSERT(fullscreen_on(OUTPUT_A) && fullscreen_on(OUTPUT_B));
  TEST_ASSERT(!fullscreen_focused_output(NULL));
  // An empty workspace may then be anywhere.
  commit(window, NULL, false, true);
  fullscreen_settle();
  TEST_ASSERT(!fullscreen_on(OUTPUT_A) && !fullscreen_on(OUTPUT_B));
}

int main(void) {
  static int manager_identity;
  manager = (struct zwlr_foreign_toplevel_manager_v1 *)&manager_identity;
  wl_list_init(&toplevels);
  test_multi_monitor();
  test_empty_workspaces();
  test_changes();
  test_without_output_events();
  reset();
  return 0;
}
