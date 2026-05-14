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
#include <condition_variable>
#include <deque>

// The notification worker runs on a dedicated thread so that a stalled
// notification daemon (e.g. elementaryOS io.elementary.notifications)
// cannot block the main loop or media key handling.
//
// Notifications are queued (max 2 entries). When multiple notifications
// pile up while the daemon is slow, intermediate ones are dropped but
// the most recent one is always preserved and shown. This ensures the
// final "now playing" notification is never lost.
struct notify_t
{
	struct entry
	{
		std::string title;
		std::string message;
		std::string image;
	};

	notify_t()
	{
		notify_init(APP_NAME);
		this->nn = notify_notification_new(nullptr, nullptr, nullptr);

		notify_notification_set_urgency(this->nn, NOTIFY_URGENCY_NORMAL);
		notify_notification_set_timeout(this->nn, NOTIFY_EXPIRES_DEFAULT);

		this->worker = std::thread(&notify_t::run, this);
	}

	~notify_t()
	{
		{
			std::lock_guard<std::mutex> lock(this->mtx);
			this->done = true;
		}
		this->cv.notify_one();

		if (this->worker.joinable())
		{
			this->worker.join();
		}

		notify_notification_close(this->nn, nullptr);
		g_object_unref(G_OBJECT(this->nn));
		notify_uninit();
	}

	void send(const std::string& title, const std::string& message, const std::string& image)
	{
		{
			std::lock_guard<std::mutex> lock(this->mtx);

			// Keep at most 1 pending entry. If there's already a pending
			// notification waiting, replace it (we only care about the latest).
			// But if the worker is currently showing one, this becomes the "next"
			// one to show — guaranteeing it will be displayed.
			if (this->queue.size() >= 2)
			{
				// Replace the last queued entry (keep the one being shown).
				this->queue.back() = {title, message, image};
			}
			else
			{
				this->queue.push_back({title, message, image});
			}
		}
		this->cv.notify_one();
	}

private:
	void run()
	{
		while (true)
		{
			std::unique_lock<std::mutex> lock(this->mtx);
			this->cv.wait(lock, [this]{ return !this->queue.empty() || this->done; });

			if (this->done && this->queue.empty())
			{
				break;
			}

			// Take the front entry.
			entry e = std::move(this->queue.front());
			this->queue.pop_front();
			lock.unlock();

			// This call may block if the daemon is unresponsive — that's fine,
			// it only blocks this worker thread, not the main loop.
			notify_notification_update(this->nn, e.title.c_str(), e.message.c_str(), e.image.c_str());

			GError* error = nullptr;
			if (!notify_notification_show(this->nn, &error))
			{
				if (error)
				{
					LOG(warning) << "notification show failed: " << error->message;
					g_error_free(error);
				}
			}
		}
	}

	std::mutex mtx;
	std::condition_variable cv;
	std::thread worker;
	std::deque<entry> queue;
	bool done = false;

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

	this->n->send(title, message, radiotray_ng::word_expand(image));
}
