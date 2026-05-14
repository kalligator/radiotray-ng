// Copyright 2017 Edward G. Bruck <ed.bruck1@gmail.com>
//
// This file is part of Radiotray-NG.
//
// Radiotray-NG is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Radiotray-NG is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Radiotray-NG.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <radiotray-ng/i_config.hpp>
#include <radiotray-ng/i_event_bus.hpp>
#include <radiotray-ng/i_player.hpp>

#include <gst/gst.h>
#include <atomic>


// Internal helper: a single GStreamer playbin pipeline instance.
struct Pipeline
{
	GstElement* playbin = nullptr;
	GstElement* souphttpsrc = nullptr;
	GstClock*   clock = nullptr;
	GstClockID  clock_id = nullptr;
	GstBus*     bus = nullptr;
	bool        buffering = false;
	bool        has_played = false;
	playlist_t  current_playlist;

	// Percentage threshold at which we consider buffering "ready" (for pending pipeline).
	int         buffer_ready_threshold = 100;
};


class Player final : public IPlayer
{
public:
	Player(std::shared_ptr<IConfig> config, std::shared_ptr<IEventBus> event_bus);

	virtual ~Player();

	bool play(const playlist_t& playlist) override;

	void stop() override;

	void volume(uint32_t percent) override;

	void mute() override;

	void unmute() override;

	bool is_muted() override;

	bool prepare(const playlist_t& playlist) override;

	bool activate() override;

	void cancel_prepare() override;

	bool is_pending_ready() override;

private:
	// Pipeline lifecycle
	bool create_pipeline(Pipeline& p);
	void destroy_pipeline(Pipeline& p);
	bool start_pipeline(Pipeline& p, const playlist_t& playlist);
	void stop_pipeline(Pipeline& p, bool publish_stopped);

	bool play_next(Pipeline& p);

	// GStreamer callbacks for the active pipeline
	static gboolean handle_messages_cb(GstBus* bus, GstMessage* message, gpointer user_data);
	static gboolean timer_cb(GstClock* clock, GstClockTime time, GstClockID id, gpointer user_data);
	static gboolean notify_volume_cb(GstBus* bus, GstMessage* message, gpointer user_data);
	static void for_each_tag_cb(const GstTagList* list, const gchar* tag, gpointer user_data);

	// GStreamer callbacks for the pending pipeline (no tags, no state events except pending_ready)
	static gboolean handle_pending_messages_cb(GstBus* bus, GstMessage* message, gpointer user_data);
	static gboolean pending_timer_cb(GstClock* clock, GstClockTime time, GstClockID id, gpointer user_data);

	Pipeline active;
	Pipeline pending;
	std::atomic<bool> pending_ready{false};

	std::shared_ptr<IEventBus> event_bus;
	std::shared_ptr<IConfig> config;
};
