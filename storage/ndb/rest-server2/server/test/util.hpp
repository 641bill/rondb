/*
 * Copyright (C) 2024 Hopsworks AB
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#ifndef STORAGE_NDB_REST_SERVER2_SERVER_TEST_UTIL_HPP_
#define STORAGE_NDB_REST_SERVER2_SERVER_TEST_UTIL_HPP_
#include <queue>
#include <mutex>
#include <condition_variable>

template <typename T>
class SafeQueue {
private:
	std::queue<T> queue_;
	std::mutex mutex_;
	std::condition_variable cond_;

public:
	void push(T value) {
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.push(std::move(value));
		cond_.notify_one();
	}

	T pop() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this]{ return !queue_.empty(); });
		T value = std::move(queue_.front());
		queue_.pop();
		return value;
	}
};

const std::string HOPSWORKS_TEST_API_KEY = "bkYjEz6OTZyevbqt.ocHajJhnE0ytBh8zbYj3IXupyMqeMZp8PW464eTxzxqP5afBjodEQUgY0lmL33ub";

#endif // STORAGE_NDB_REST_SERVER2_SERVER_TEST_UTIL_HPP_
