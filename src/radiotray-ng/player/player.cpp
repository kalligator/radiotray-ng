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

#include <radiotray-ng/player/player.hpp>
#include <cmath>


Player::Player(std::shared_ptr<IConfig> config, std::shared_ptr<IEventBus> event_bus)
	: event_bus(std::move(event_bus))
	, config(std::move(config))
{
	LOG(info) << "starting gstreamer";

	gst_init(nullptr, nullptr);

	if (!this->create_pipeline(this->active))
	{
		LOG(error) << "failed to create active pipeline";
	}
}


Player::~Player()
{
	LOG(info) << "stopping gstreamer";

	this->destroy_pipeline(this->pending);
	this->destroy_pipeline(this->active);
	gst_deinit();
}


bool Player::create_pipeline(Pipeline& p)
{
	if ((p.playbin = gst_element_factory_make("playbin3", nullptr)) == nullptr)
	{
		LOG(warning) << "could not create playbin3 element, falling back to playbin";

		if ((p.playbin = gst_element_factory_make("playbin", nullptr)) == nullptr)
		{
			LOG(error) << "could not create playbin element";
			return false;
		}

		LOG(warning) << "m3u8 support will not be available using playbin";
	}

	if ((p.souphttpsrc = gst_element_factory_make("souphttpsrc", nullptr)) == nullptr)
	{
		LOG(error) << "could not create souphttpsrc element";
		gst_object_unref(p.playbin);
		p.playbin = nullptr;
		return false;
	}

	GstElement* audio_sink;
	if ((audio_sink = gst_element_factory_make("autoaudiosink", nullptr)) == nullptr)
	{
		LOG(error) << "could not create autoaudiosink element";
		gst_object_unref(p.playbin);
		gst_object_unref(p.souphttpsrc);
		p.playbin = nullptr;
		p.souphttpsrc = nullptr;
		return false;
	}

	g_object_set(p.playbin, "audio-sink", audio_sink, NULL);

	p.clock = gst_pipeline_get_clock(GST_PIPELINE(p.playbin));
	p.bus = gst_element_get_bus(p.playbin);

	return true;
}


void Player::destroy_pipeline(Pipeline& p)
{
	if (p.bus)
	{
		gst_bus_remove_watch(p.bus);
		gst_object_unref(p.bus);
		p.bus = nullptr;
	}

	if (p.clock_id)
	{
		gst_clock_id_unschedule(p.clock_id);
		gst_clock_id_unref(p.clock_id);
		p.clock_id = nullptr;
	}

	if (p.clock)
	{
		gst_object_unref(G_OBJECT(p.clock));
		p.clock = nullptr;
	}

	if (p.playbin)
	{
		gst_element_set_state(p.playbin, GST_STATE_NULL);
		gst_object_unref(p.playbin);
		p.playbin = nullptr;
	}

	if (p.souphttpsrc)
	{
		gst_element_set_state(p.souphttpsrc, GST_STATE_NULL);
		gst_object_unref(p.souphttpsrc);
		p.souphttpsrc = nullptr;
	}

	p.buffering = false;
	p.has_played = false;
	p.current_playlist.clear();
}


void Player::stop_pipeline(Pipeline& p, bool publish_stopped)
{
	if (!p.playbin)
	{
		return;
	}

	GstState state;
	gst_element_get_state(GST_ELEMENT(p.playbin), &state, nullptr, GST_CLOCK_TIME_NONE);

	if (state != GST_STATE_NULL)
	{
		gst_element_set_state(GST_ELEMENT(p.playbin), GST_STATE_NULL);
		p.buffering = false;

		if (p.clock_id)
		{
			LOG(debug) << "canceling outstanding clock request";
			gst_clock_id_unschedule(p.clock_id);
			gst_clock_id_unref(p.clock_id);
			p.clock_id = nullptr;
		}

		if (publish_stopped)
		{
			this->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_STOPPED);
		}
	}
}


bool Player::play_next(Pipeline& p)
{
	this->stop_pipeline(p, false);

	if (!p.current_playlist.empty())
	{
		LOG(debug) << "uri: " << p.current_playlist.front();

		g_object_set(p.playbin, "uri", p.current_playlist.front().c_str(), NULL);

		p.current_playlist.erase(p.current_playlist.begin());

		const uint32_t buffer_size = this->config->get_uint32(BUFFER_SIZE_KEY, DEFAULT_BUFFER_SIZE_VALUE);
		const uint32_t buffer_duration = this->config->get_uint32(BUFFER_DURATION_KEY, DEFAULT_BUFFER_DURATION_VALUE);

		g_object_set(G_OBJECT(p.playbin), "buffer-size", buffer_size * buffer_duration, NULL);
		g_object_set(G_OBJECT(p.playbin), "buffer-duration", buffer_duration * GST_SECOND, NULL);

		LOG(debug) << BUFFER_SIZE_KEY << "=" << std::to_string(buffer_size * buffer_duration)
			<< ", " << BUFFER_DURATION_KEY << "=" << buffer_duration;

		if (!p.has_played)
		{
			const auto vol = this->config->get_uint32(VOLUME_LEVEL_KEY, DEFAULT_VOLUME_LEVEL_VALUE);
			LOG(debug) << "setting startup volume: " << vol;
			g_object_set(G_OBJECT(p.playbin), "volume", vol / 100.0, NULL);
		}

		if (gst_element_set_state(p.playbin, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
		{
			LOG(error) << "Failed to set pipeline to: GST_STATE_PAUSED";
			return false;
		}

		return true;
	}

	LOG(info) << "playlist is empty";
	return false;
}


bool Player::start_pipeline(Pipeline& p, const playlist_t& playlist)
{
	if (!p.playbin)
	{
		LOG(error) << "pipeline not initialized";
		return false;
	}

	if (playlist.empty())
	{
		LOG(error) << "playlist is empty";
		return false;
	}

	p.current_playlist = playlist;

	if (!this->play_next(p))
	{
		this->event_bus->publish_only(IEventBus::event::station_error, ERROR_KEY,
			"Unable to set the pipeline to the playing state!");
		return false;
	}

	return true;
}


// === Public IPlayer interface ===

bool Player::play(const playlist_t& playlist)
{
	if (!this->active.bus)
	{
		LOG(error) << "gstreamer not ready";
		return false;
	}

	// Cancel any pending prepare
	this->cancel_prepare();

	// Install active bus watch
	gst_bus_add_watch(this->active.bus, static_cast<GstBusFunc>(&Player::handle_messages_cb), this);
	g_signal_connect(this->active.playbin, "notify::volume", G_CALLBACK(&Player::notify_volume_cb), this);

	return this->start_pipeline(this->active, playlist);
}


void Player::stop()
{
	this->cancel_prepare();
	this->stop_pipeline(this->active, true);
}


void Player::volume(const uint32_t percent)
{
	gdouble vol{percent / 100.0};

	if (this->active.playbin)
	{
		g_object_set(G_OBJECT(this->active.playbin), "volume", vol, NULL);
	}

	this->event_bus->publish_only(IEventBus::event::volume_changed, VOLUME_LEVEL_KEY, std::to_string(percent));
	this->config->set_uint32(VOLUME_LEVEL_KEY, percent);
}


void Player::mute()
{
	if (this->active.playbin)
	{
		g_object_set(G_OBJECT(this->active.playbin), "mute", TRUE, NULL);
	}
}


void Player::unmute()
{
	if (this->active.playbin)
	{
		g_object_set(G_OBJECT(this->active.playbin), "mute", FALSE, NULL);
	}
}


bool Player::is_muted()
{
	if (!this->active.playbin)
	{
		return false;
	}

	gboolean muted{};
	g_object_get(G_OBJECT(this->active.playbin), "mute", &muted, NULL);
	return !!muted;
}


bool Player::prepare(const playlist_t& playlist)
{
	LOG(info) << "preparing pending pipeline for seamless switch";

	// Tear down any existing pending pipeline
	this->cancel_prepare();

	// Create a fresh pending pipeline
	if (!this->create_pipeline(this->pending))
	{
		LOG(error) << "failed to create pending pipeline";
		return false;
	}

	// Mute the pending pipeline — it should buffer without producing audio
	g_object_set(G_OBJECT(this->pending.playbin), "mute", TRUE, NULL);

	// Match the active pipeline's volume so activate() just unmutes
	const auto vol = this->config->get_uint32(VOLUME_LEVEL_KEY, DEFAULT_VOLUME_LEVEL_VALUE);
	g_object_set(G_OBJECT(this->pending.playbin), "volume", vol / 100.0, NULL);
	this->pending.has_played = true;  // skip startup volume logic

	// Set buffer ready threshold from config (default 90%)
	this->pending.buffer_ready_threshold = 90;

	// Install the pending bus watch (different callback — no tags, no state events to UI)
	gst_bus_add_watch(this->pending.bus, static_cast<GstBusFunc>(&Player::handle_pending_messages_cb), this);

	this->pending_ready = false;

	// Start buffering
	this->pending.current_playlist = playlist;

	if (!this->play_next(this->pending))
	{
		LOG(error) << "failed to start pending pipeline";
		this->destroy_pipeline(this->pending);
		return false;
	}

	return true;
}


bool Player::activate()
{
	if (!this->pending_ready || !this->pending.playbin)
	{
		LOG(warning) << "activate called but pending pipeline not ready";
		return false;
	}

	LOG(info) << "activating pending pipeline (seamless switch)";

	// Stop the active pipeline (no state_changed event — we'll publish PLAYING from the new one)
	this->stop_pipeline(this->active, false);

	// Remove the bus watch from old active
	if (this->active.bus)
	{
		gst_bus_remove_watch(this->active.bus);
	}

	// Destroy old active pipeline
	this->destroy_pipeline(this->active);

	// Swap pending → active
	this->active = this->pending;

	// Zero out pending struct (moved)
	this->pending = Pipeline{};
	this->pending_ready = false;

	// Remove pending bus watch and install active bus watch
	gst_bus_remove_watch(this->active.bus);
	gst_bus_add_watch(this->active.bus, static_cast<GstBusFunc>(&Player::handle_messages_cb), this);
	g_signal_connect(this->active.playbin, "notify::volume", G_CALLBACK(&Player::notify_volume_cb), this);

	// Unmute and ensure it's playing
	g_object_set(G_OBJECT(this->active.playbin), "mute", FALSE, NULL);
	gst_element_set_state(GST_ELEMENT(this->active.playbin), GST_STATE_PLAYING);

	// Publish playing state
	this->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_PLAYING);

	return true;
}


void Player::cancel_prepare()
{
	if (this->pending.playbin)
	{
		LOG(debug) << "canceling pending pipeline";
		this->destroy_pipeline(this->pending);
		this->pending = Pipeline{};
		this->pending_ready = false;
	}
}


bool Player::is_pending_ready()
{
	return this->pending_ready;
}


// === GStreamer callbacks for ACTIVE pipeline ===

gboolean Player::timer_cb(GstClock* /*clock*/, GstClockTime /*time*/, GstClockID /*id*/, gpointer user_data)
{
	auto player{static_cast<Player*>(user_data)};

	gst_clock_id_unref(player->active.clock_id);
	player->active.clock_id = nullptr;

	if (player->active.buffering)
	{
		LOG(error) << "buffering timeout, restarting stream...";

		gst_element_set_state(player->active.playbin, GST_STATE_NULL);
		gst_element_set_state(player->active.souphttpsrc, GST_STATE_NULL);
		gst_element_set_state(player->active.playbin, GST_STATE_PAUSED);
	}

	return TRUE;
}


gboolean Player::notify_volume_cb(GstBus* /*bus*/, GstMessage* /*message*/, gpointer user_data)
{
	auto player{static_cast<Player*>(user_data)};

	if (!player->active.playbin)
	{
		return TRUE;
	}

	gdouble volume;
	g_object_get(G_OBJECT(player->active.playbin), "volume", &volume, NULL);

	const uint32_t new_volume = std::round(volume * 100);

	if (player->config->get_uint32(VOLUME_LEVEL_KEY, DEFAULT_VOLUME_LEVEL_VALUE) != new_volume)
	{
		LOG(debug) << "volume: " << new_volume;

		player->config->set_uint32(VOLUME_LEVEL_KEY, new_volume);
		player->config->save();

		player->event_bus->publish_only(IEventBus::event::volume_changed, VOLUME_LEVEL_KEY, std::to_string(new_volume));
	}

	return TRUE;
}


gboolean Player::handle_messages_cb(GstBus* /*bus*/, GstMessage* message, gpointer user_data)
{
	auto player{static_cast<Player*>(user_data)};
	auto& p = player->active;

	switch (GST_MESSAGE_TYPE(message))
	{
		case GST_MESSAGE_ERROR:
		{
			GError* err;
			gchar* debug_info;
			gst_message_parse_error(message, &err, &debug_info);

			LOG(error) << "error received from element " << GST_OBJECT_NAME(message->src) << ": " << err->message
				<< " , " << int(err->domain) << ":" << int(err->code);
			LOG(error) << "debugging information: " << ((debug_info) ? debug_info : "none");

			gst_element_set_state(p.playbin, GST_STATE_NULL);
			gst_element_set_state(p.souphttpsrc, GST_STATE_NULL);

			if (err->domain == GST_RESOURCE_ERROR && err->code == GST_RESOURCE_ERROR_SEEK)
			{
				LOG(error) << "dropped connection, restarting stream...";
				gst_element_set_state(p.playbin, GST_STATE_PAUSED);
				p.buffering = true;
			}
			else
			{
				if (!player->play_next(p))
				{
					LOG(debug) << "setting state to: " << STATE_STOPPED;
					player->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_STOPPED);
					player->event_bus->publish_only(IEventBus::event::station_error, ERROR_KEY, err->message);
				}
			}

			g_clear_error(&err);
			g_free(debug_info);
		}
		break;

		case GST_MESSAGE_EOS:
		{
			LOG(debug) << "end-of-stream reached";

			if (!player->play_next(p))
			{
				LOG(debug) << "setting state to: " << STATE_STOPPED;
				player->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_STOPPED);
			}
		}
		break;

		case GST_MESSAGE_BUFFERING:
		{
			gint percent;
			gst_message_parse_buffering(message, &percent);

			if (percent == 100)
			{
				p.buffering = false;
				LOG(debug) << "stopped buffering, setting state to: GST_STATE_PLAYING";
				gst_element_set_state(GST_ELEMENT(p.playbin), GST_STATE_PLAYING);
			}
			else
			{
				if (!p.buffering)
				{
					LOG(debug) << "started buffering, setting state to: GST_STATE_PAUSED";
					gst_element_set_state(GST_ELEMENT(p.playbin), GST_STATE_PAUSED);
				}
				p.buffering = true;
			}
		}
		break;

		case GST_MESSAGE_TAG:
		{
			if (player->event_bus)
			{
				IEventBus::event_data_t notify_data;

				GstTagList* tags;
				gst_message_parse_tag(message, &tags);
				gst_tag_list_foreach(tags, static_cast<GstTagForeachFunc>(&Player::for_each_tag_cb), &notify_data);
				gst_tag_list_free(tags);

				player->event_bus->publish(IEventBus::event::tags_changed, notify_data);
			}
		}
		break;

		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState old_state;
			GstState new_state;
			gst_message_parse_state_changed(message, &old_state, &new_state, nullptr);
			p.has_played = true;

			if (GST_MESSAGE_SRC(message) == GST_OBJECT(p.playbin))
			{
				if (new_state == GST_STATE_PLAYING)
				{
					player->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_PLAYING);
				}
				else if (new_state == GST_STATE_PAUSED)
				{
					if (p.clock_id)
					{
						LOG(info) << "canceling outstanding clock request";
						gst_clock_id_unschedule(p.clock_id);
						gst_clock_id_unref(p.clock_id);
						p.clock_id = nullptr;
					}

					if (!p.buffering)
					{
						break;
					}

					player->event_bus->publish_only(IEventBus::event::state_changed, STATE_KEY, STATE_BUFFERING);

					p.clock_id = gst_clock_new_single_shot_id(p.clock, gst_clock_get_time(p.clock) + (10 * GST_SECOND));
					gst_clock_id_wait_async(p.clock_id, static_cast<GstClockCallback>(&Player::timer_cb), player, nullptr);
				}
			}
		}
		break;

		default:
			break;
	}

	return TRUE;
}


// === GStreamer callbacks for PENDING pipeline ===

gboolean Player::pending_timer_cb(GstClock* /*clock*/, GstClockTime /*time*/, GstClockID /*id*/, gpointer user_data)
{
	auto player{static_cast<Player*>(user_data)};

	if (player->pending.clock_id)
	{
		gst_clock_id_unref(player->pending.clock_id);
		player->pending.clock_id = nullptr;
	}

	if (player->pending.buffering)
	{
		LOG(error) << "pending pipeline buffering timeout, discarding";

		// Publish error so RadiotrayNG knows the prepare failed
		player->event_bus->publish_only(IEventBus::event::station_error, ERROR_KEY,
			"New station buffering timed out");
	}

	return TRUE;
}


gboolean Player::handle_pending_messages_cb(GstBus* /*bus*/, GstMessage* message, gpointer user_data)
{
	auto player{static_cast<Player*>(user_data)};
	auto& p = player->pending;

	if (!p.playbin)
	{
		return TRUE;
	}

	switch (GST_MESSAGE_TYPE(message))
	{
		case GST_MESSAGE_ERROR:
		{
			GError* err;
			gchar* debug_info;
			gst_message_parse_error(message, &err, &debug_info);

			LOG(error) << "pending pipeline error: " << err->message;
			LOG(error) << "debugging information: " << ((debug_info) ? debug_info : "none");

			// Try next URL in the pending playlist
			gst_element_set_state(p.playbin, GST_STATE_NULL);
			gst_element_set_state(p.souphttpsrc, GST_STATE_NULL);

			if (!player->play_next(p))
			{
				LOG(error) << "pending pipeline failed, signaling error";
				player->event_bus->publish_only(IEventBus::event::station_error, ERROR_KEY,
					"New station failed to connect");
			}

			g_clear_error(&err);
			g_free(debug_info);
		}
		break;

		case GST_MESSAGE_BUFFERING:
		{
			gint percent;
			gst_message_parse_buffering(message, &percent);

			if (percent >= p.buffer_ready_threshold)
			{
				if (!player->pending_ready)
				{
					p.buffering = false;
					player->pending_ready = true;

					LOG(info) << "pending pipeline buffered at " << percent << "%, ready to activate";

					// Signal RadiotrayNG that the pending stream is ready
					player->event_bus->publish_only(IEventBus::event::pending_ready, STATE_KEY, "ready");
				}

				// Let it continue buffering/playing in muted state
				gst_element_set_state(GST_ELEMENT(p.playbin), GST_STATE_PLAYING);
			}
			else
			{
				if (!p.buffering)
				{
					LOG(debug) << "pending pipeline buffering at " << percent << "%";
					gst_element_set_state(GST_ELEMENT(p.playbin), GST_STATE_PAUSED);
				}
				p.buffering = true;
			}
		}
		break;

		case GST_MESSAGE_STATE_CHANGED:
		{
			GstState new_state;
			gst_message_parse_state_changed(message, nullptr, &new_state, nullptr);

			if (GST_MESSAGE_SRC(message) == GST_OBJECT(p.playbin))
			{
				if (new_state == GST_STATE_PAUSED && p.buffering)
				{
					// Start buffering timeout for pending pipeline
					if (p.clock_id)
					{
						gst_clock_id_unschedule(p.clock_id);
						gst_clock_id_unref(p.clock_id);
						p.clock_id = nullptr;
					}

					p.clock_id = gst_clock_new_single_shot_id(p.clock, gst_clock_get_time(p.clock) + (15 * GST_SECOND));
					gst_clock_id_wait_async(p.clock_id, static_cast<GstClockCallback>(&Player::pending_timer_cb), player, nullptr);
				}
			}
		}
		break;

		default:
			break;
	}

	return TRUE;
}


// === Tag parsing (shared) ===

void Player::for_each_tag_cb(const GstTagList* list, const gchar* tag, gpointer user_data)
{
	auto& event_data = *static_cast<IEventBus::event_data_t*>(user_data);

	const guint count = gst_tag_list_get_tag_size(list, tag);

	for (guint i = 0; i < count; i++)
	{
		gchar* str{nullptr};

		if (gst_tag_get_type(tag) == G_TYPE_STRING)
		{
			if (!gst_tag_list_get_string_index(list, tag, i, &str))
			{
				LOG(error) << "gst_tag_list_get_string_index failed for tag: " << gst_tag_get_nick(tag);
				g_free(str);
				continue;
			}
		}
		else
		{
			str = g_strdup_value_contents(gst_tag_list_get_value_index(list, tag, i));
		}

		// Ignore anything that looks encoded...
		if (std::string(str).find("<?xml") == std::string::npos)
		{
			event_data[gst_tag_get_nick(tag)] = str;
		}
		else
		{
			LOG(debug) << "ignoring encoded tag: " << gst_tag_get_nick(tag) << " : " << str;
		}

		g_free(str);
	}
}
