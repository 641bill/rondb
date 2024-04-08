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

#include <mutex>
#include <iostream>

#ifndef STORAGE_NDB_REST_SERVER2_SERVER_SRC_STATISTICS_HPP_
#define STORAGE_NDB_REST_SERVER2_SERVER_SRC_STATISTICS_HPP_

class TimingStatistics {
	std::chrono::microseconds totalPreparationTime{0};
	std::chrono::microseconds totalParsingTime{0};
	std::chrono::microseconds totalValidationTime{0};
	std::chrono::microseconds totalRequestCreationTime{0};
	std::chrono::microseconds totalPKReadTime{0};
	std::chrono::microseconds totalResponseCreationTime{0};
	std::chrono::microseconds totalRequestHandlingTime{0};
	unsigned long requestCount{0};
	mutable std::mutex mutex;

public:
	TimingStatistics() = default;

	~TimingStatistics() {
		print_statistics();
	}

	void update(const std::chrono::microseconds& prepTime,
							const std::chrono::microseconds& parseTime,
							const std::chrono::microseconds& validTime,
							const std::chrono::microseconds& requestCreationTime,
							const std::chrono::microseconds& pkReadTime,
							const std::chrono::microseconds& responseCreationTime,
							const std::chrono::microseconds& requestHandlingTime) {
		std::lock_guard<std::mutex> lock(mutex);
		totalPreparationTime += prepTime;
		totalParsingTime += parseTime;
		totalValidationTime += validTime;
		totalRequestCreationTime += requestCreationTime;
		totalPKReadTime += pkReadTime;
		totalResponseCreationTime += responseCreationTime;
		totalRequestHandlingTime += requestHandlingTime;
		++requestCount;
	}

	void print_statistics() const {
		std::lock_guard<std::mutex> lock(mutex);
		if (requestCount == 0) return;
		std::cout << "Average Preparation Time: " << totalPreparationTime.count() / requestCount << "us\n";
		std::cout << "Average Parsing Time: " << totalParsingTime.count() / requestCount << "us\n";
		std::cout << "Average Validation Time: " << totalValidationTime.count() / requestCount << "us\n";
		std::cout << "Average Request Creation Time: " << totalRequestCreationTime.count() / requestCount << "us\n";
		std::cout << "Average Read Time: " << totalPKReadTime.count() / requestCount << "us\n";
		std::cout << "Average Response Creation Time: " << totalResponseCreationTime.count() / requestCount << "us\n";
		std::cout << "Average Request Handling Time: " << totalRequestHandlingTime.count() / requestCount << "us\n";
	}
};

extern TimingStatistics timingStatistics;

#endif // STORAGE_NDB_REST_SERVER2_SERVER_SRC_STATISTICS_HPP_
