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
#include <atomic>

// The notification worker runs on a dedicated thread so that a stalled
// notification daemon (e.g. elementaryOS io.elementary.notifications)
// cannot block the main loop or media key handling.
struct notify_t
{
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
			// Always overwrite — only the latest notification matters.
			this->pending_title = title;
			this->pending_message = message;
			this->pending_image = image;
			this->has_pending = true;
		}
		this->cv.notify_one();
	}

private:
	void run()
	{
		while (true)
		{
			std::unique_lock<std::mutex> lock(this->mtx);
			this->cv.wait(lock, [this]{ return this->has_pending || this->done; });

			if (this->done && !this->has_pending)
			{
				break;
			}

			// Grab the latest pending notification.
			std::string title = std::move(this->pending_title);
			std::string message = std::move(this->pending_message);
			std::string image = std::move(this->pending_image);
			this->has_pending = false;
			lock.unlock();

			// This call may block if the daemon is unresponsive — that's fine,
			// it only blocks this worker thread, not the main loop.
			notify_notification_update(this->nn, title.c_str(), message.c_str(), image.c_str());

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
	bool has_pending = false;
	bool done = false;

	std::string pending_title;
	std::string pending_message;
	std::string pending_image;

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
