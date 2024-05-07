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

#include "../src/api_key.hpp"
#include "../src/config_structs.hpp"
#include "connection.hpp"
#include "resources/embeddings.hpp"
#include "rdrs_dal.h"
#include "rdrs_hopsworks_dal.h"
#include "rdrs_rondb_connection.hpp"
#include "rdrs_rondb_connection_pool.hpp"
#include "src/constants.hpp"
#include "util.hpp"

#include <cstdio>
#include <drogon/HttpClient.h>
#include <drogon/HttpResponse.h>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <chrono>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

class BatchSubOp {
public:
	std::string method;
	std::string relativeUrl;
	PKReadBody body;
};

class BatchSubOperationTestInfo {
public:
	BatchSubOp subOperation;
	std::string table;
	std::string db;
	std::vector<int> httpCode; // for some operations there are multiple valid return codes
	std::vector<std::string> respKVs;
};

struct HttpResponse {
	int httpCode;
	std::vector<char> httpBody;
};

HttpResponse send_http_request_with_client(drogon::HttpClientPtr client, std::string httpVerb, std::string url, std::string body, std::string expectedErrMsg, std::vector<int> expectedStatus) {
	auto request = drogon::HttpRequest::newHttpRequest();
	auto response = drogon::HttpResponse::newHttpResponse();
	if (httpVerb == POST) {
		request->setMethod(drogon::Post);
		request->setBody(body);
		request->setPath(url);
		request->setContentTypeCode(drogon::ContentType::CT_APPLICATION_JSON);
	} else if (httpVerb == GET) {
		request->setMethod(drogon::Get);
		request->setPath(url);
	} else {
		FAIL() << "HTTP verb " << httpVerb << " is not supported";
	}

	if (globalConfigs.security.apiKey.useHopsworksAPIKeys) {
		request->addHeader(API_KEY_NAME, HOPSWORKS_TEST_API_KEY);
	}

	client->sendRequest(request, [response, &url, &body](drogon::ReqResult result, const drogon::HttpResponsePtr& resp) {
		if (result == drogon::ReqResult::Ok) {
			response->setStatusCode(resp->getStatusCode());
			response->setBody(std::string(resp->getBody()));
		} else {
			FAIL() << "failed to perform HTTP request towards url: " << url << " \nrequest body: " << body << "\nerror: " << result;
		}
	});

	int idx = -1;

	for (auto i = 0; i < expectedStatus.size(); i++) {
		if (response->getStatusCode() == expectedStatus[i]) {
			idx = i;
		}
	}

	if (idx == -1) {
		FAIL() << "received unexpected status '" << response->getStatusCode() << "'\nexpected status: " << expectedStatus;
	}



	

}

HttpResponse send_http_request(std::string httpVerb, std::string url, std::string body, std::string expectedErrMsg, std::vector<int> expectedStatus) {
	auto client = drogon::HttpClient::newHttpClient(globalConfigs.rest.serverIP, globalConfigs.rest.serverPort);

}

void batch_rest_test_with_client(drogon::HttpClientPtr client, BatchSubOperationTestInfo testInfo, bool isBinarydaata, bool validateData) {

}

void batch_rest_test(BatchSubOperationTestInfo testInfo, bool isBinaryData, bool validateData) {
	auto client = drogon::HttpClient::newHttpClient(globalConfigs.rest.serverIP, globalConfigs.rest.serverPort);
	
}

class BatchOperationTestInfo {
public:
	std::vector<BatchSubOperationTestInfo> operations;
	std::vector<int> httpCode; // for some operations there are multiple valid return codes
	std::string errMsgContains;
};

class ReconnectionTest : public ::testing::Test {
protected:
	static void SetUpTestSuite() {
		RS_Status status = RonDBConnection::init_rondb_connection(globalConfigs.ronDB, globalConfigs.ronDbMetaDataCluster);
		if (status.http_code != static_cast<HTTP_CODE>(drogon::HttpStatusCode::k200OK)) {
			errno = status.http_code;
			exit(errno);
		}
	}

	static void TearDownTestSuite() {
		RS_Status status = RonDBConnection::shutdown_rondb_connection();
		if (status.http_code != static_cast<HTTP_CODE>(drogon::HttpStatusCode::k200OK)) {
			errno = status.http_code;
			exit(errno);
		}
	}
};

void batchPKWorker(int id, std::unordered_map<std::string, BatchOperationTestInfo>& tests, std::atomic<bool>* stop, SafeQueue<int>* done) {
	auto httpClient = drogon::HttpClient::newHttpClient(globalConfigs.rest.serverIP, globalConfigs.rest.serverPort);

	int opCount = 0;
	// need to do this if an operation fails




	// Simulate doing work as in the Go code, periodically checking *stop
	int opCount = 0;
	// When done:
	done->push(opCount);
}

void statWorker(std::atomic<bool>* stop) {
    // Setup httpClient, etc.
    // Simulate doing work as in the Go code, periodically checking *stop
}

void reconnectionTest(int numThreads, int durationSec, std::unordered_map<std::string, BatchOperationTestInfo>) {
	bool stop = false;
	SafeQueue<int> done;

	std::vector<std::thread> threads;
	for (int i = 0; i < numThreads; ++i) {
		threads.push_back(std::thread(batchPKWorker, i, std::ref(tests), &stop, &done));
		// Assuming you want a statWorker per thread for demonstration
		threads.push_back(std::thread(statWorker, &stop));
	}

}

TEST_F(ReconnectionTest, TestReconnection1) {
	// TODO logger

}

TEST_F(ReconnectionTest, TestReconnection2) {
}

