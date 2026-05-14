# Radiotray-NG Session Context

## Repository
- Fork: `kalligator/radiotray-ng` (forked from `ebruck/radiotray-ng`)
- User also uses: `IngoMeyer441/radiotray-ng-mpris` (MPRIS D-Bus bridge)
- User's DE: elementaryOS 8.1 (Pantheon/wingpanel)
- User's icon theme: `kpm-icons` at `~/.local/share/icons/kpm-icons/` (inherits elementary)

## Active Branches

### PR #1: `feature/late-stop-station-switching`
**Conservative approach** — good for upstream submission. Changes:
1. **Late-stop**: Delay `player->stop()` until after playlist download resolves. Config: `"late-stop": true`
2. **Debounced next/prev**: 150ms settle window on background thread. Config: `"station-switch-delay": 150`
3. **Async notifications**: Dedicated worker thread with max-2 deque queue
4. **Notification images**: Uses `file://` URI for absolute paths, `notify_notification_clear_hints()` between notifications
5. **Failed station stays on old**: If download fails while playing, old station keeps going + error notification
6. **User icon theme paths**: `gtk_icon_theme_prepend_search_path()` + `app_indicator_set_icon_theme_path()`

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
- `PlaylistDownloader::download_playlist()` is synchronous (curl) — blocks caller

## Config Keys Added
| Key | Default | Description |
|-----|---------|-------------|
| `late-stop` | `true` | Delay stop until playlist ready |
| `seamless-switching` | `true` | Dual-pipeline (PR #2 only) |
| `station-switch-delay` | `150` | Debounce ms for next/prev |

## Icon Theme Status (IMPORTANT for next session)
- **Panel icons (wingpanel)**: User uses ABSOLUTE PATHS in config as workaround:
  ```json
  "radiotray-ng-on": "/home/xeiristis/.local/share/icons/kpm-icons/apps/22/radiotray-ng-on.svg",
  "radiotray-ng-off": "/home/xeiristis/.local/share/icons/kpm-icons/apps/22/radiotray-ng-off.svg"
  ```
- **GTK icon theme resolves correctly** (verified via python3 `Gtk.IconTheme.lookup_icon`) — `kpm-icons` theme has icons at `apps/24/radiotray-ng-on.svg` etc.
- **`app_indicator_set_icon_theme_path()` HURTS** — it overrides normal theme lookup with a flat directory search. Should be REMOVED in next session. Without it, wingpanel should use normal GTK theme resolution. The user's absolute paths in config bypass the issue entirely for now.
- **Bare icon names ("radiotray-ng-on") DON'T WORK** in wingpanel even though GTK finds them — likely because `app_indicator_set_icon_theme_path()` is interfering. Removing that call is the fix to try next session.
- **Station notification images**: Work correctly with `file://` URI + `clear_hints()`. User has absolute paths in bookmarks.json `"image"` field. No dimension requirements.
- **Apps menu icon**: User has local `.desktop` file with `Icon=radiotray` — resolves from theme fine after icon cache rebuild.

## TODO for Next Session
1. **Remove `app_indicator_set_icon_theme_path()`** from both branches — it interferes with normal wingpanel theme resolution. Test if bare icon names work without it (they should, now that icon cache is valid).
2. **Notification overlay issue**: Verify the `file://` URI + `clear_hints()` approach eliminated the miniature overlay icon and stale image problems.
3. **Upstream submission**: User wants to clean up the late-stop branch for an upstream PR to `ebruck/radiotray-ng`. The seamless branch stays on the personal fork.
4. **Seamless branch testing**: User hasn't tested PR #2 yet — needs to build and verify the dual-pipeline approach works.

## User's Station Image Setup
- Absolute paths in bookmarks.json: `"image": "/home/xeiristis/.radios/rainbow89.png"`
- Images are arbitrary dimensions (daemon scales them)
- Stations without images use the config's `radiotray-ng-notification` value

## Files Modified (both branches)
- `include/radiotray-ng/common.hpp` — config keys + defaults
- `include/radiotray-ng/i_player.hpp` — new virtual methods (PR #2)
- `include/radiotray-ng/i_event_bus.hpp` — `pending_ready` event (PR #2)
- `src/radiotray-ng/radiotray_ng.hpp` — debounce members, seamless state
- `src/radiotray-ng/radiotray_ng.cpp` — play() rewritten, debounce worker, pending_ready handler
- `src/radiotray-ng/player/player.hpp` — Pipeline struct, dual pipeline (PR #2)
- `src/radiotray-ng/player/player.cpp` — refactored for dual pipeline (PR #2)
- `src/radiotray-ng/notification/linux/notification.cpp` — async worker, image fixes
- `src/radiotray-ng/gui/appindicator/appindicator_gui.cpp` — icon theme paths (needs cleanup)
- `.kiro/session-context.md` — this file
