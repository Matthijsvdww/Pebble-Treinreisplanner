#include <pebble.h>

typedef enum {
  KEY_MSG_TYPE = 0, KEY_ERROR = 1, KEY_ITEM_COUNT = 10,
  KEY_UPDATED = 24,
  KEY_T0_TIME = 40, KEY_T0_TRAIN = 41, KEY_T0_TRACK = 42, KEY_T0_DELAY = 43,
  KEY_T1_TIME = 44, KEY_T1_TRAIN = 45, KEY_T1_TRACK = 46, KEY_T1_DELAY = 47,
  KEY_T2_TIME = 48, KEY_T2_TRAIN = 49, KEY_T2_TRACK = 50, KEY_T2_DELAY = 51,
  KEY_T3_TIME = 52, KEY_T3_TRAIN = 53, KEY_T3_TRACK = 54, KEY_T3_DELAY = 55,
  KEY_T4_TIME = 76, KEY_T4_TRAIN = 77, KEY_T4_TRACK = 78, KEY_T4_DELAY = 79,
  KEY_T5_TIME = 80, KEY_T5_TRAIN = 81, KEY_T5_TRACK = 82, KEY_T5_DELAY = 83,
  KEY_T6_TIME = 84, KEY_T6_TRAIN = 85, KEY_T6_TRACK = 86, KEY_T6_DELAY = 87,
  KEY_T7_TIME = 88, KEY_T7_TRAIN = 89, KEY_T7_TRACK = 90, KEY_T7_DELAY = 91,
  KEY_SEL_TIME = 60, KEY_SEL_TRAIN = 61, KEY_CANCEL = 62,
  KEY_NEXT_ROUTE = 63, KEY_VIBRATE_AT = 65, KEY_ROUTE_LABEL = 70,
  KEY_TIMELINE = 71, KEY_HEAD = 72, KEY_HEAD_STYLE = 73, KEY_LEG_IDX = 74,
  KEY_WARN_CODE = 75, KEY_DISRUPTION = 96,
} AppKey;

#define MAX_TRIPS 8

// timeline layout
#define TL_W 200
#define ROW_H 18
#define MAX_ROWS 28

// snapshot (instant restore)
#define SNAP_MAGIC      0x4E534D31   // "NSM1"
#define SNAP_META_KEY   2
#define SNAP_TL_KEY     3            // timeline chunks: keys 3..7
#define SNAP_CHUNK      224
#define SNAP_CHUNKS     5
#define SNAP_MAX_AGE_S  (12 * 3600)

static Window *s_menu_window, *s_card_window;
static MenuLayer *s_menu;
static TextLayer *s_menu_status;
static bool s_card_on_top = false;
static char s_route_label[24];
static int32_t s_vibrate_at = 0;
static bool s_vibrated = false;

// card: fixed strips + custom-drawn scrollable timeline
static ScrollLayer *s_scroll;
static TextLayer *s_title, *s_clock, *s_head, *s_status;
static Layer *s_tl;
static GFont s_tl_font, s_tl_font_b;
static GColor s_tl_bg, s_tl_fg, s_tl_dim;
static char s_row_l[MAX_ROWS][56];   // left text per row
static char s_row_r[MAX_ROWS][16];   // right text per row (track / crowd code)
static int  s_row_y[MAX_ROWS];       // y position per row
static int  s_row_h[MAX_ROWS];       // height per row
static int  s_row_n = 0;
static int  s_leg_row = -1;          // timeline row of current leg header
static int  s_scrolled_row = -1;     // last row we auto-scrolled to
static char s_head_buf[56];
static char s_timeline_buf[1100];
static char s_status_buf[36];

// snapshot + warning state
static bool s_card_cancelled = false;
static bool s_restore_pending = false;
static int32_t s_last_warn = 0;
static time_t s_last_snap = 0;

static int p_count = 0;
static char p_time[MAX_TRIPS][8];
static char p_cat[MAX_TRIPS][8];
static char p_num[MAX_TRIPS][8];
static char p_track[MAX_TRIPS][6];
static int  p_delay[MAX_TRIPS];

static char row_title[MAX_TRIPS][24];
static char row_sub[MAX_TRIPS][24];

static void copy_str(char *dst, int n, const char *src) {
  snprintf(dst, n, "%s", src ? src : "");
}

// ---------- TRANSFER VIBRATION ----------
static void do_transfer_vibrate(void) {
  static const uint32_t seg[] = { 400, 150, 400, 150, 400 };
  VibePattern pat = { .durations = seg, .num_segments = ARRAY_LENGTH(seg) };
  vibes_enqueue_custom_pattern(pat);
}
static void check_vibrate(void) {
  if (s_vibrated || s_vibrate_at == 0) return;
  if ((int32_t)time(NULL) >= s_vibrate_at) {
    s_vibrated = true;
    s_vibrate_at = 0;
    do_transfer_vibrate();
  }
}

// ---------- CLOCK ----------
static void update_clock(void) {
  static char buf[8];
  clock_copy_time_string(buf, sizeof(buf));
  text_layer_set_text(s_clock, buf);
}
static void tick_handler(struct tm *tick_time, TimeUnits units) {
  update_clock();
  check_vibrate();
}

// ---------- MENU ----------
static uint16_t menu_num_rows(MenuLayer *menu_layer, uint16_t section_index, void *ctx) {
  return p_count > 0 ? (uint16_t)(p_count + 1) : 1; // row 0 = route
}
static void menu_draw_row(GContext *gctx, const Layer *layer, MenuIndex *idx, void *ctx) {
  if (p_count == 0) {
    menu_cell_basic_draw(gctx, layer, "Laden...", NULL, NULL);
    return;
  }
  if (idx->row == 0) {
    menu_cell_basic_draw(gctx, layer, "Route:", s_route_label, NULL);
    return;
  }
  int i = idx->row - 1;
  snprintf(row_title[i], sizeof(row_title[i]), "%s  %s %s", p_time[i], p_cat[i], p_num[i]);
  if (p_delay[i] > 0)
    snprintf(row_sub[i], sizeof(row_sub[i]), "sp.%s  +%d min", p_track[i], p_delay[i]);
  else
    snprintf(row_sub[i], sizeof(row_sub[i]), "sp.%s  op tijd", p_track[i]);
  menu_cell_basic_draw(gctx, layer, row_title[i], row_sub[i], NULL);
}
static void up_click_handler(ClickRecognizerRef ref, void *ctx) {
  menu_layer_set_selected_next(s_menu, true, MenuRowAlignCenter, true);
}
static void down_click_handler(ClickRecognizerRef ref, void *ctx) {
  menu_layer_set_selected_next(s_menu, false, MenuRowAlignCenter, true);
}
static void select_click_handler(ClickRecognizerRef ref, void *ctx) {
  if (p_count == 0) return;
  MenuIndex idx = menu_layer_get_selected_index(s_menu);
  if (idx.row == 0) {
    static int one = 1;
    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
      dict_write_int(iter, KEY_NEXT_ROUTE, &one, sizeof(int), true);
      app_message_outbox_send();
    }
    return;
  }
  if (idx.row > p_count) return;
  int i = idx.row - 1;
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_cstring(iter, KEY_SEL_TIME, p_time[i]);
    dict_write_cstring(iter, KEY_SEL_TRAIN, p_num[i]);
    app_message_outbox_send();
  }
  window_stack_push(s_card_window, true);
}
static void menu_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
}

// ---------- CARD BUTTONS ----------
static void card_back_handler(ClickRecognizerRef ref, void *ctx) {
  s_card_cancelled = true;   // tracking stops; drop the snapshot
  DictionaryIterator *iter;
  static int one = 1;
  if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
    dict_write_int(iter, KEY_CANCEL, &one, sizeof(int), true);
    app_message_outbox_send();
  }
  window_stack_pop(s_card_window);
}
static void card_long_select_handler(ClickRecognizerRef ref, void *ctx) {
  window_stack_pop_all(true);   // exit; card_unload saves the snapshot
}
static void card_click_config_provider(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_BACK, card_back_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, card_long_select_handler, NULL);
}

// ---------- CARD: timeline ----------
static void timeline_parse(const char *src) {
  s_row_n = 0;
  const char *p = src;
  while (*p && s_row_n < MAX_ROWS) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *pipe = memchr(p, '|', len);
    if (pipe) {
      size_t ll = (size_t)(pipe - p);
      if (ll >= sizeof(s_row_l[0])) ll = sizeof(s_row_l[0]) - 1;
      memcpy(s_row_l[s_row_n], p, ll);
      s_row_l[s_row_n][ll] = 0;
      size_t rl = len - ll - 1;
      if (rl >= sizeof(s_row_r[0])) rl = sizeof(s_row_r[0]) - 1;
      memcpy(s_row_r[s_row_n], pipe + 1, rl);
      s_row_r[s_row_n][rl] = 0;
    } else {
      if (len >= sizeof(s_row_l[0])) len = sizeof(s_row_l[0]) - 1;
      memcpy(s_row_l[s_row_n], p, len);
      s_row_l[s_row_n][len] = 0;
      s_row_r[s_row_n][0] = 0;
    }
    s_row_n++;
    if (!nl) break;
    p = nl + 1;
  }
}

static void timeline_scroll_to_leg(void) {
  if (s_leg_row < 0 || s_leg_row >= s_row_n || s_leg_row == s_scrolled_row) return;
  s_scrolled_row = s_leg_row;
  GRect frame = layer_get_frame(scroll_layer_get_layer(s_scroll));
  int total = s_row_y[s_row_n - 1] + s_row_h[s_row_n - 1];
  int max = total - frame.size.h;
  int off = s_row_y[s_leg_row];
  if (off > max) off = max;
  if (off < 0) off = 0;
  scroll_layer_set_content_offset(s_scroll, GPoint(0, -off), true);
}

static void timeline_apply(void) {
  timeline_parse(s_timeline_buf);
  int y = 0;
  for (int i = 0; i < s_row_n; i++) {
    bool is_hdr  = strstr(s_row_l[i], " richting ") != NULL;
    bool is_xfer = strstr(s_row_l[i], "overstap") != NULL;
    if (is_hdr || is_xfer || !s_row_r[i][0]) {
      // full-width row: measure so long text wraps instead of clipping
      GFont f = is_hdr ? s_tl_font_b : s_tl_font;
      GRect box = GRect(0, 0, is_hdr ? TL_W - 10 : TL_W, 2000);
      GSize sz = graphics_text_layout_get_content_size(
          s_row_l[i], f, box, GTextOverflowModeWordWrap,
          is_xfer ? GTextAlignmentCenter : GTextAlignmentLeft);
      int h = (int)sz.h;
      if (h < ROW_H) h = ROW_H;
      h = ((h + ROW_H - 1) / ROW_H) * ROW_H;   // snap to row grid
      s_row_h[i] = h;
    } else {
      s_row_h[i] = ROW_H;                       // stop rows: fixed height
    }
    s_row_y[i] = y;
    y += s_row_h[i];
  }
  int total = y > 0 ? y : ROW_H;
  layer_set_frame(s_tl, GRect(0, 0, TL_W, total));
  scroll_layer_set_content_size(s_scroll, GSize(TL_W, total));
  layer_mark_dirty(s_tl);
  timeline_scroll_to_leg();
}

static void tl_update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, s_tl_bg);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
  for (int i = 0; i < s_row_n; i++) {
    int y = s_row_y[i], h = s_row_h[i];
    bool is_hdr  = strstr(s_row_l[i], " richting ") != NULL;
    bool is_xfer = strstr(s_row_l[i], "overstap") != NULL;
    bool is_warn = s_row_l[i][0] == '!';
    GColor c = s_tl_fg;
    GFont f = s_tl_font;
    if (is_warn)      { c = GColorRed; }
    else if (is_hdr)  { c = GColorBlue; f = s_tl_font_b; }
    else if (is_xfer) { c = s_tl_dim; }
    else if (strncmp(s_row_l[i], "Punctualiteit", 13) == 0) c = GColorGreen;
    else if (strstr(s_row_r[i], ">") || strstr(s_row_l[i], " +")) c = GColorOrange;

    graphics_context_set_text_color(ctx, c);
    if (is_hdr) {
      // crowd color bar: right side carries G / O / R
      GColor bar = GColorGreen;
      bool has_bar = true;
      if (s_row_r[i][0] == 'O') bar = GColorOrange;
      else if (s_row_r[i][0] == 'R') bar = GColorRed;
      else if (s_row_r[i][0] != 'G') has_bar = false;
      if (has_bar) {
        graphics_context_set_fill_color(ctx, bar);
        graphics_fill_rect(ctx, GRect(0, y + 2, 6, h - 4), 0, GCornerNone);
      }
      graphics_draw_text(ctx, s_row_l[i], f, GRect(10, y, TL_W - 10, h),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    } else if (!s_row_r[i][0]) {
      graphics_draw_text(ctx, s_row_l[i], f, GRect(0, y, TL_W, h),
                         GTextOverflowModeWordWrap,
                         is_xfer ? GTextAlignmentCenter : GTextAlignmentLeft, NULL);
    } else {
      // stop row: text left, track hard right
      int rw = strstr(s_row_r[i], ">") ? 56 : 30;
      graphics_draw_text(ctx, s_row_l[i], f, GRect(0, y, TL_W - rw, h),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      graphics_draw_text(ctx, s_row_r[i], f, GRect(TL_W - rw, y, rw, h),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
    }
  }
}

// ---------- SNAPSHOT (instant restore) ----------
typedef struct {
  uint32_t magic;
  uint32_t epoch;
  int32_t  warn;
  char     head[56];
  char     status[36];
  char     label[24];
  uint16_t tl_len;
  uint8_t  chunks;
} SnapMeta;

static void snapshot_clear(void) {
  for (int k = SNAP_META_KEY; k < SNAP_TL_KEY + SNAP_CHUNKS; k++)
    persist_delete(k);
  s_last_snap = 0;
}

static void snapshot_save(bool force) {
  time_t now = time(NULL);
  if (!force && s_last_snap != 0 && now - s_last_snap < 600) return; // max 1x/10min
  size_t tl_len = strlen(s_timeline_buf) + 1;
  if (tl_len > sizeof(s_timeline_buf)) tl_len = sizeof(s_timeline_buf);

  SnapMeta m;
  memset(&m, 0, sizeof(m));
  m.magic = SNAP_MAGIC;
  m.epoch = (uint32_t)now;
  m.warn  = s_last_warn;
  copy_str(m.head, sizeof(m.head), s_head_buf);
  copy_str(m.status, sizeof(m.status), s_status_buf);
  copy_str(m.label, sizeof(m.label), s_route_label);
  m.tl_len = (uint16_t)tl_len;
  m.chunks = (uint8_t)((tl_len + SNAP_CHUNK - 1) / SNAP_CHUNK);

  if (persist_write_data(SNAP_META_KEY, &m, sizeof(m)) != S_TRUE) return;
  for (uint8_t i = 0; i < m.chunks && i < SNAP_CHUNKS; i++) {
    size_t off = i * SNAP_CHUNK;
    size_t n = tl_len - off;
    if (n > SNAP_CHUNK) n = SNAP_CHUNK;
    if (persist_write_data(SNAP_TL_KEY + i, s_timeline_buf + off, n) != S_TRUE) return;
  }
  s_last_snap = now;
}

static bool snapshot_load(void) {
  SnapMeta m;
  if (persist_get_size(SNAP_META_KEY) != (int)sizeof(m)) return false;
  if (persist_read_data(SNAP_META_KEY, &m, sizeof(m)) != (int)sizeof(m)) return false;
  if (m.magic != SNAP_MAGIC) return false;
  if ((time_t)m.epoch < time(NULL) - SNAP_MAX_AGE_S) { snapshot_clear(); return false; }

  memset(s_timeline_buf, 0, sizeof(s_timeline_buf));
  for (uint8_t i = 0; i < m.chunks && i < SNAP_CHUNKS; i++) {
    size_t off = i * SNAP_CHUNK;
    size_t cap = sizeof(s_timeline_buf) - off - 1;
    size_t n = (m.tl_len - off > SNAP_CHUNK) ? SNAP_CHUNK : (m.tl_len - off);
    if (n > cap) n = cap;
    if (persist_read_data(SNAP_TL_KEY + i, s_timeline_buf + off, n) < 0) {
      snapshot_clear();
      return false;
    }
  }
  s_timeline_buf[sizeof(s_timeline_buf) - 1] = 0;

  copy_str(s_head_buf, sizeof(s_head_buf), m.head);
  copy_str(s_status_buf, sizeof(s_status_buf), m.status);
  copy_str(s_route_label, sizeof(s_route_label), m.label);
  persist_write_string(1, s_route_label);
  s_last_warn = m.warn;   // no re-buzz for a warning you already saw
  return true;
}

// ---------- INBOX ----------
static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *type = dict_find(iter, KEY_MSG_TYPE);
  if (!type) return;
  int32_t mt = type->value->int32;

  if (mt == 1) { // picker list
    s_card_cancelled = true;   // this pop must not save a snapshot
    if (s_card_on_top) { window_stack_pop(s_card_window); }
    s_card_cancelled = false;
    snapshot_clear();
    s_last_warn = 0;
    Tuple *rl = dict_find(iter, KEY_ROUTE_LABEL);
    if (rl) {
      copy_str(s_route_label, sizeof(s_route_label), rl->value->cstring);
      persist_write_string(1, s_route_label);
    }
    Tuple *cnt = dict_find(iter, KEY_ITEM_COUNT);
    p_count = cnt ? (int)cnt->value->int32 : 0;
    if (p_count > MAX_TRIPS) p_count = MAX_TRIPS;
    // trip keys are NOT contiguous (76-91 has a gap after 55),
    // so use a table instead of KEY_T0_TIME + i * 4
    static const uint32_t trip_keys[8][4] = {
      { KEY_T0_TIME, KEY_T0_TRAIN, KEY_T0_TRACK, KEY_T0_DELAY },
      { KEY_T1_TIME, KEY_T1_TRAIN, KEY_T1_TRACK, KEY_T1_DELAY },
      { KEY_T2_TIME, KEY_T2_TRAIN, KEY_T2_TRACK, KEY_T2_DELAY },
      { KEY_T3_TIME, KEY_T3_TRAIN, KEY_T3_TRACK, KEY_T3_DELAY },
      { KEY_T4_TIME, KEY_T4_TRAIN, KEY_T4_TRACK, KEY_T4_DELAY },
      { KEY_T5_TIME, KEY_T5_TRAIN, KEY_T5_TRACK, KEY_T5_DELAY },
      { KEY_T6_TIME, KEY_T6_TRAIN, KEY_T6_TRACK, KEY_T6_DELAY },
      { KEY_T7_TIME, KEY_T7_TRAIN, KEY_T7_TRACK, KEY_T7_DELAY }
    };
    for (int i = 0; i < p_count && i < 8; i++) {
      Tuple *t;
      t = dict_find(iter, trip_keys[i][0]);
      if (t) copy_str(p_time[i], sizeof(p_time[i]), t->value->cstring);
      t = dict_find(iter, trip_keys[i][1]);
      if (t) {
        const char *s = t->value->cstring;
        char *sp = strchr(s, ' ');
        if (sp) {
          snprintf(p_cat[i], (int)(sp - s) + 1, "%s", s);
          copy_str(p_num[i], sizeof(p_num[i]), sp + 1);
        } else {
          copy_str(p_cat[i], sizeof(p_cat[i]), s);
          copy_str(p_num[i], sizeof(p_num[i]), "");
        }
      }
      t = dict_find(iter, trip_keys[i][2]);
      if (t) copy_str(p_track[i], sizeof(p_track[i]), t->value->cstring);
      t = dict_find(iter, trip_keys[i][3]);
      if (t) p_delay[i] = (int)t->value->int32;
    }
    menu_layer_reload_data(s_menu);
    Tuple *dis = dict_find(iter, KEY_DISRUPTION);
    if (dis) {
      text_layer_set_text(s_menu_status, dis->value->cstring);
    } else {
      text_layer_set_text(s_menu_status, p_count > 0 ? "Kies je trein:" : "Geen reizen");
    }
    s_vibrated = false; s_vibrate_at = 0;
    s_leg_row = -1; s_scrolled_row = -1;
  } else if (mt == 2 || mt == 3) { // card (waiting OR riding)
    if (!s_card_on_top) window_stack_push(s_card_window, true);
    Tuple *rl = dict_find(iter, KEY_ROUTE_LABEL);
    if (rl) {
      copy_str(s_route_label, sizeof(s_route_label), rl->value->cstring);
      persist_write_string(1, s_route_label);
      text_layer_set_text(s_title, s_route_label);
    }
    Tuple *head = dict_find(iter, KEY_HEAD);
    Tuple *hsty = dict_find(iter, KEY_HEAD_STYLE);
    if (head) {
      copy_str(s_head_buf, sizeof(s_head_buf), head->value->cstring);
      text_layer_set_text(s_head, s_head_buf);
      int st = (hsty && hsty->type == TUPLE_INT) ? (int)hsty->value->int32 : 0;
      text_layer_set_text_color(s_head, st == 2 ? GColorRed : st == 1 ? GColorOrange : s_tl_fg);
    }
    // auto-scroll target: only during the ride
    s_leg_row = -1;
    s_scrolled_row = -1;
    if (mt == 3) {
      Tuple *li = dict_find(iter, KEY_LEG_IDX);
      if (li && li->type == TUPLE_INT) s_leg_row = (int)li->value->int32;
    }
    Tuple *tl = dict_find(iter, KEY_TIMELINE);
    if (tl) {
      copy_str(s_timeline_buf, sizeof(s_timeline_buf), tl->value->cstring);
      timeline_apply();
    }
    Tuple *upd = dict_find(iter, KEY_UPDATED);
    if (upd) {
      snprintf(s_status_buf, sizeof(s_status_buf), "Upd %s", upd->value->cstring);
      text_layer_set_text(s_status, s_status_buf);
    }
    // warning vibration: buzz when a (new) warning appears
    Tuple *wc = dict_find(iter, KEY_WARN_CODE);
    int code = (wc && wc->type == TUPLE_INT) ? (int)wc->value->int32 : 0;
    if (code != 0 && code != s_last_warn) vibes_double_pulse();
    s_last_warn = code;
    // keep a fresh snapshot on flash (rate-limited) in case the app dies
    snapshot_save(false);
    Tuple *vat = dict_find(iter, KEY_VIBRATE_AT);
    if (vat && vat->type == TUPLE_INT) s_vibrate_at = vat->value->int32;
    check_vibrate(); // buzz immediately if the target already passed
  } else if (mt == 4) {
    Tuple *err = dict_find(iter, KEY_ERROR);
    if (s_card_on_top) text_layer_set_text(s_status, err ? err->value->cstring : "Fout");
    else text_layer_set_text(s_menu_status, err ? err->value->cstring : "Fout");
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "msg dropped: %d", (int)reason);
}

// ---------- WINDOWS ----------
static void menu_load(Window *window) {
  Layer *root = window_get_root_layer(s_menu_window);
  GRect bounds = layer_get_bounds(root);
  s_menu = menu_layer_create(GRect(0, 0, bounds.size.w, bounds.size.h - 22));
  window_set_click_config_provider(s_menu_window, menu_click_config_provider);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks) {
    .get_num_rows = menu_num_rows,
    .draw_row = menu_draw_row
  });
  layer_add_child(root, menu_layer_get_layer(s_menu));
  s_menu_status = text_layer_create(GRect(0, bounds.size.h - 22, bounds.size.w, 22));
  text_layer_set_font(s_menu_status, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text(s_menu_status, "Laden...");
  text_layer_set_text_alignment(s_menu_status, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_menu_status));
}
static void menu_unload(Window *window) {
  menu_layer_destroy(s_menu);
  text_layer_destroy(s_menu_status);
}

static void card_load(Window *window) {
  s_card_on_top = true;
  Layer *root = window_get_root_layer(s_card_window);
  GRect bounds = layer_get_bounds(root);

  // light theme (black on white)
  window_set_background_color(s_card_window, GColorWhite);
  s_tl_bg = GColorWhite;
  s_tl_fg = GColorBlack;
  s_tl_dim = GColorDarkGray;
  s_tl_font   = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  s_tl_font_b = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

  s_title = text_layer_create(GRect(0, 0, bounds.size.w - 66, 18));
  text_layer_set_font(s_title, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_color(s_title, s_tl_fg);
  text_layer_set_text(s_title, s_route_label);
  layer_add_child(root, text_layer_get_layer(s_title));

  s_clock = text_layer_create(GRect(bounds.size.w - 64, 0, 64, 18));
  text_layer_set_font(s_clock, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_color(s_clock, s_tl_fg);
  text_layer_set_text_alignment(s_clock, GTextAlignmentRight);
  layer_add_child(root, text_layer_get_layer(s_clock));
  update_clock();

  s_head = text_layer_create(GRect(0, 19, bounds.size.w, 27));
  text_layer_set_font(s_head, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_head, GTextAlignmentCenter);
  text_layer_set_text_color(s_head, s_tl_fg);
  text_layer_set_text(s_head, "--");
  layer_add_child(root, text_layer_get_layer(s_head));

  s_scroll = scroll_layer_create(GRect(0, 48, bounds.size.w, bounds.size.h - 68));
  scroll_layer_set_shadow_hidden(s_scroll, true);
  s_tl = layer_create(GRect(0, 0, TL_W, MAX_ROWS * ROW_H));
  layer_set_update_proc(s_tl, tl_update_proc);
  scroll_layer_add_child(s_scroll, s_tl);
  layer_add_child(root, scroll_layer_get_layer(s_scroll));

  s_status = text_layer_create(GRect(0, bounds.size.h - 20, bounds.size.w, 20));
  text_layer_set_font(s_status, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_status, GTextAlignmentCenter);
  text_layer_set_text_color(s_status, s_tl_fg);
  text_layer_set_text(s_status, "");
  layer_add_child(root, text_layer_get_layer(s_status));

  // ScrollLayer owns UP/DOWN; our provider adds BACK + long-SELECT
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks) {
    .click_config_provider = card_click_config_provider
  });
  scroll_layer_set_click_config_onto_window(s_scroll, s_card_window);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);

  // instant restore: paint the snapshot before the phone wakes up
  if (s_restore_pending) {
    s_restore_pending = false;
    text_layer_set_text(s_title, s_route_label);
    text_layer_set_text(s_head, s_head_buf);
    text_layer_set_text(s_status, s_status_buf);
    timeline_apply();
  }
}

static void card_unload(Window *window) {
  s_card_on_top = false;
  tick_timer_service_unsubscribe();
  if (s_card_cancelled) {
    snapshot_clear();          // tracking stopped; nothing to restore
  } else {
    snapshot_save(true);       // exit with an active trip -> save for reopen
  }
  s_card_cancelled = false;
  text_layer_destroy(s_title);
  text_layer_destroy(s_clock);
  text_layer_destroy(s_head);
  text_layer_destroy(s_status);
  layer_destroy(s_tl);
  scroll_layer_destroy(s_scroll);
}

static void init(void) {
  if (persist_read_string(1, s_route_label, sizeof(s_route_label)) == 0)
    snprintf(s_route_label, sizeof(s_route_label), "Route");
  bool restore = snapshot_load();
  s_menu_window = window_create();
  window_set_window_handlers(s_menu_window, (WindowHandlers) {
    .load = menu_load, .unload = menu_unload });
  s_card_window = window_create();
  window_set_window_handlers(s_card_window, (WindowHandlers) {
    .load = card_load, .unload = card_unload });
  app_message_open(app_message_inbox_size_maximum(), 256);
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  window_stack_push(s_menu_window, true);
  if (restore) {
    s_restore_pending = true;
    window_stack_push(s_card_window, true);
  }
}

static void deinit(void) {
  window_destroy(s_card_window);
  window_destroy(s_menu_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
