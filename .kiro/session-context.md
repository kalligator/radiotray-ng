# Radiotray-NG Session Context

## Repository
- Fork: `kalligator/radiotray-ng` (forked from `ebruck/radiotray-ng`)
- User also uses: `IngoMeyer441/radiotray-ng-mpris` (MPRIS D-Bus bridge)
- User's DE: elementaryOS 8.1 (Pantheon/wingpanel)

## Active Branches

### PR #1: `feature/late-stop-station-switching`
**Conservative approach** — good for upstream submission. Changes:
1. **Late-stop**: Delay `player->stop()` until after playlist download resolves. Config: `"late-stop": true`
2. **Debounced next/prev**: 150ms settle window on background thread. Config: `"station-switch-delay": 150`
3. **Async notifications**: Dedicated worker thread with max-2 deque queue
4. **Notification images**: Uses `file://` URI for absolute paths, `notify_notification_clear_hints()` between notifications
5. **Failed station stays on old**: If download fails while playing, old station keeps going + error notification
6. **User icon theme priority**: `gtk_icon_theme_prepend_search_path()` + `app_indicator_set_icon_theme_path()` for wingpanel

### PR #2: `feature/seamless-dual-pipeline`
**Full seamless switching** — more radical, probably not for upstream. Includes everything from PR #1 plus:
1. **Dual GStreamer pipelines**: Second `playbin` buffers new station muted in background
2. **Zero-gap swap**: When pending reaches 100% buffer, fires `pending_ready` event → `activate()` stops old, unmutes new
3. **Fallback chain**: seamless → late-stop → classic (all configurable)
4. **New IPlayer methods**: `prepare()`, `activate()`, `cancel_prepare()`, `is_pending_ready()`
5. **New event**: `IEventBus::event::pending_ready`
6. Config: `"seamless-switching": true`

## Key Architecture Notes
- Player uses GStreamer `playbin3` (falls back to `playbin`)
- Bus messages dispatched on GTK main loop (`gst_bus_add_watch`)
- GStreamer clock callbacks run on their own thread (timer_cb, pending_timer_cb)
- Notification daemon (`io.elementary.notifications`) is buggy — blocks D-Bus calls when overloaded
- `app_indicator_set_icon()` on elementaryOS goes through wingpanel's D-Bus → needs `set_icon_theme_path()` for user-local themes
- `PlaylistDownloader::download_playlist()` is synchronous (curl) — blocks caller

## Config Keys Added
| Key | Default | Description |
|-----|---------|-------------|
| `late-stop` | `true` | Delay stop until playlist ready |
| `seamless-switching` | `true` | Dual-pipeline (PR #2 only) |
| `station-switch-delay` | `150` | Debounce ms for next/prev |

## Known Issues / TODO
- **Icon theme on wingpanel**: `app_indicator_set_icon_theme_path()` added — needs user testing. The path should be the parent directory containing the theme folders (e.g. `~/.local/share/icons`), and the icon files must follow freedesktop icon-theme-spec structure.
- **Notification overlay**: Fixed by using `file://` URI + `clear_hints()` — needs user verification that the miniature overlay is gone.
- **Upstream submission plan**: User wants to clean up branches for upstream PR. Late-stop branch is more conservative. Seamless branch is too radical for upstream but good for personal fork.

## Files Modified (both branches)
- `include/radiotray-ng/common.hpp` — config keys + defaults
- `include/radiotray-ng/i_player.hpp` — new virtual methods (PR #2)
- `include/radiotray-ng/i_event_bus.hpp` — `pending_ready` event (PR #2)
- `src/radiotray-ng/radiotray_ng.hpp` — debounce members, seamless state
- `src/radiotray-ng/radiotray_ng.cpp` — play() rewritten, debounce worker, pending_ready handler
- `src/radiotray-ng/player/player.hpp` — Pipeline struct, dual pipeline (PR #2)
- `src/radiotray-ng/player/player.cpp` — refactored for dual pipeline (PR #2)
- `src/radiotray-ng/notification/linux/notification.cpp` — async worker, image fixes
- `src/radiotray-ng/gui/appindicator/appindicator_gui.cpp` — icon theme paths
