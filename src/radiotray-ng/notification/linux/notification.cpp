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

#include <radiotray-ng/common.hpp>
#include <radiotray-ng/notification/notification.hpp>
#include <libnotify/notify.h>
#include <mutex>
#include <thread>
#include <chrono>
#include <future>

// lazy pimpl...
struct notify_t
{
	notify_t()
	{
		notify_init(APP_NAME);
		this->nn = notify_notification_new(nullptr, nullptr, nullptr);

		notify_notification_set_urgency(this->nn, NOTIFY_URGENCY_NORMAL);
		notify_notification_set_timeout(this->nn, NOTIFY_EXPIRES_DEFAULT);
	}

	~notify_t()
	{
		notify_notification_close(this->nn, nullptr);
		g_object_unref(G_OBJECT(this->nn));
		notify_uninit();
	}

	std::mutex mtx;
	NotifyNotification* nn;
};


Notification::Notification()
	: n(new notify_t())
{
}


Notification::~Notification()
{
}


void Notification::notify(const std::string& title, const std::string& message)
{
	this->notify(title, message, "");
}


void Notification::notify(const std::string& title, const std::string& message, const std::string& image)
{
	LOG(debug) << "notify: " << title << ", " << message << ", " << image;

	// Attempt to acquire the lock without blocking. If a previous notification
	// call is still stuck in the daemon, skip this one rather than blocking the
	// main loop (which would stall media key processing).
	std::unique_lock<std::mutex> lock(this->n->mtx, std::try_to_lock);

	if (!lock.owns_lock())
	{
		LOG(warning) << "notification daemon busy, skipping notification";
		return;
	}

	notify_notification_update(this->n->nn, title.c_str(), message.c_str(), radiotray_ng::word_expand(image).c_str());

	GError* error = nullptr;
	if (!notify_notification_show(this->n->nn, &error))
	{
		if (error)
		{
			LOG(warning) << "notification show failed: " << error->message;
			g_error_free(error);
		}
	}
}
