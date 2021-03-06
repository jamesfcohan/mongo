/**
 *    Copyright 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/jsobj.h"
#include "mongo/db/repl/fetcher.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"

#include "mongo/unittest/unittest.h"

namespace {

    using namespace mongo;
    using namespace mongo::repl;
    using executor::NetworkInterfaceMock;

    const HostAndPort target("localhost", -1);
    const BSONObj findCmdObj = BSON("find" << "coll");

    class FetcherTest : public ReplicationExecutorTest {
    public:
        static Status getDetectableErrorStatus();
        FetcherTest();
        void setUp() override;
        void tearDown() override;
        void clear();
        void scheduleNetworkResponse(const BSONObj& obj);
        void scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason);
        void processNetworkResponse(const BSONObj& obj);
        void processNetworkResponse(ErrorCodes::Error code, const std::string& reason);
        void finishProcessingNetworkResponse();

    protected:
        Status status;
        CursorId cursorId;
        Fetcher::Documents documents;
        Fetcher::NextAction nextAction;
        Fetcher::NextAction newNextAction;
        std::unique_ptr<Fetcher> fetcher;
        // Called at end of _callback
        Fetcher::CallbackFn callbackHook;

    private:
        void _callback(const StatusWith<Fetcher::BatchData>& result,
                       Fetcher::NextAction* nextAction,
                       BSONObjBuilder* getMoreBob);
    };

    Status FetcherTest::getDetectableErrorStatus() {
        return Status(ErrorCodes::InternalError, "Not mutated");
    }

    FetcherTest::FetcherTest()
        : status(getDetectableErrorStatus()),
          cursorId(-1),
          nextAction(Fetcher::NextAction::kInvalid) { }

    void FetcherTest::setUp() {
        ReplicationExecutorTest::setUp();
        clear();
        fetcher.reset(new Fetcher(
            &getExecutor(), target, "db", findCmdObj,
            stdx::bind(&FetcherTest::_callback, this,
                       stdx::placeholders::_1, stdx::placeholders::_2, stdx::placeholders::_3)));
        launchExecutorThread();
    }

    void FetcherTest::tearDown() {
        ReplicationExecutorTest::tearDown();
        // Executor may still invoke fetcher's callback before shutting down.
        fetcher.reset();
    }

    void FetcherTest::clear() {
        status = getDetectableErrorStatus();
        cursorId = -1;
        documents.clear();
        nextAction = Fetcher::NextAction::kInvalid;
    }

    void FetcherTest::scheduleNetworkResponse(const BSONObj& obj) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        Milliseconds millis(0);
        RemoteCommandResponse response(obj, millis);
        ReplicationExecutor::ResponseStatus responseStatus(response);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
    }

    void FetcherTest::scheduleNetworkResponse(ErrorCodes::Error code, const std::string& reason) {
        NetworkInterfaceMock* net = getNet();
        ASSERT_TRUE(net->hasReadyRequests());
        ReplicationExecutor::ResponseStatus responseStatus(code, reason);
        net->scheduleResponse(net->getNextReadyRequest(), net->now(), responseStatus);
    }

    void FetcherTest::processNetworkResponse(const BSONObj& obj) {
        scheduleNetworkResponse(obj);
        finishProcessingNetworkResponse();
    }

    void FetcherTest::processNetworkResponse(ErrorCodes::Error code,
                                             const std::string& reason) {
        scheduleNetworkResponse(code, reason);
        finishProcessingNetworkResponse();
    }

    void FetcherTest::finishProcessingNetworkResponse() {
        clear();
        ASSERT_TRUE(fetcher->isActive());
        getNet()->runReadyNetworkOperations();
        ASSERT_FALSE(getNet()->hasReadyRequests());
        ASSERT_FALSE(fetcher->isActive());
    }

    void FetcherTest::_callback(const StatusWith<Fetcher::BatchData>& result,
                                Fetcher::NextAction* nextActionFromFetcher,
                                BSONObjBuilder* getMoreBob) {
        status = result.getStatus();
        if (result.isOK()) {
            const Fetcher::BatchData& batchData = result.getValue();
            cursorId = batchData.cursorId;
            documents = batchData.documents;
        }

        if (callbackHook) {
            callbackHook(result, nextActionFromFetcher, getMoreBob);
        }

        if (nextActionFromFetcher) {
            nextAction = *nextActionFromFetcher;
        }
    }

    void unusedFetcherCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                               Fetcher::NextAction* nextAction,
                               BSONObjBuilder* getMoreBob) {
        FAIL("should not reach here");
    }

    TEST_F(FetcherTest, InvalidConstruction) {
        ReplicationExecutor& executor = getExecutor();

        // Null executor.
        ASSERT_THROWS(Fetcher(nullptr, target, "db", findCmdObj, unusedFetcherCallback),
                      UserException);

        // Empty database name.
        ASSERT_THROWS(Fetcher(&executor, target, "", findCmdObj, unusedFetcherCallback),
                      UserException);

        // Empty command object.
        ASSERT_THROWS(Fetcher(&executor, target, "db", BSONObj(), unusedFetcherCallback),
                      UserException);

        // Callback function cannot be null.
        ASSERT_THROWS(Fetcher(&executor, target, "db", findCmdObj, Fetcher::CallbackFn()),
                      UserException);
    }

    // Command object can refer to any command that returns a cursor. This
    // includes listIndexes and listCollections.
    TEST_F(FetcherTest, NonFindCommand) {
        ReplicationExecutor& executor = getExecutor();

        Fetcher(&executor, target, "db", BSON("listIndexes" << "coll"), unusedFetcherCallback);
        Fetcher(&executor, target, "db", BSON("listCollections" << 1), unusedFetcherCallback);
        Fetcher(&executor, target, "db", BSON("a" << 1), unusedFetcherCallback);
    }

    TEST_F(FetcherTest, GetDiagnosticString) {
        Fetcher fetcher(&getExecutor(), target, "db", findCmdObj, unusedFetcherCallback);
        ASSERT_FALSE(fetcher.getDiagnosticString().empty());
    }

    TEST_F(FetcherTest, IsActiveAfterSchedule) {
        ASSERT_FALSE(fetcher->isActive());
        ASSERT_OK(fetcher->schedule());
        ASSERT_TRUE(fetcher->isActive());
    }

    TEST_F(FetcherTest, ScheduleWhenActive) {
        ASSERT_OK(fetcher->schedule());
        ASSERT_TRUE(fetcher->isActive());
        ASSERT_NOT_OK(fetcher->schedule());
    }

    TEST_F(FetcherTest, CancelWithoutSchedule) {
        ASSERT_FALSE(fetcher->isActive());
        fetcher->cancel();
    }

    TEST_F(FetcherTest, WaitWithoutSchedule) {
        ASSERT_FALSE(fetcher->isActive());
        fetcher->wait();
    }

    TEST_F(FetcherTest, ShutdownBeforeSchedule) {
        getExecutor().shutdown();
        ASSERT_NOT_OK(fetcher->schedule());
        ASSERT_FALSE(fetcher->isActive());
    }

    TEST_F(FetcherTest, ScheduleAndCancel) {
        ASSERT_OK(fetcher->schedule());
        scheduleNetworkResponse(BSON("ok" << 1));

        fetcher->cancel();
        finishProcessingNetworkResponse();

        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
    }

    TEST_F(FetcherTest, ScheduleButShutdown) {
        ASSERT_OK(fetcher->schedule());
        scheduleNetworkResponse(BSON("ok" << 1));

        getExecutor().shutdown();
        // Network interface should not deliver mock response to callback.
        finishProcessingNetworkResponse();

        ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status.code());
    }

    TEST_F(FetcherTest, FindCommandFailed1) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(ErrorCodes::BadValue, "bad hint");
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_EQUALS("bad hint", status.reason());
    }

    TEST_F(FetcherTest, FindCommandFailed2) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("ok" << 0 <<
                                    "errmsg" << "bad hint" <<
                                    "code" << int(ErrorCodes::BadValue)));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_EQUALS("bad hint", status.reason());
    }

    TEST_F(FetcherTest, CursorFieldMissing) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor' field");
    }

    TEST_F(FetcherTest, CursorNotAnObject) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << 123 << "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor' field must be an object");
    }

    TEST_F(FetcherTest, CursorIdFieldMissing) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.id' field");
    }

    TEST_F(FetcherTest, CursorIdNotLongNumber) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 123.1 <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(),
                               "'cursor.id' field must be");
        ASSERT_EQ((int)Fetcher::NextAction::kInvalid, (int)nextAction);
    }

    TEST_F(FetcherTest, NamespaceFieldMissing) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.ns' field");
    }

    TEST_F(FetcherTest, NamespaceNotAString) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << 123 <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' field must be a string");
    }

    TEST_F(FetcherTest, NamespaceEmpty) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << "" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
    }

    TEST_F(FetcherTest, NamespaceMissingCollectionName) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 123LL <<
                                                     "ns" << "db." <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::BadValue, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.ns' contains an invalid namespace");
    }

    TEST_F(FetcherTest, FirstBatchFieldMissing) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll") <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "must contain 'cursor.firstBatch' field");
    }

    TEST_F(FetcherTest, FirstBatchNotAnArray) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << 123) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "'cursor.firstBatch' field must be an array");
    }

    TEST_F(FetcherTest, FirstBatchArrayContainsNonObject) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSON_ARRAY(8)) <<
                                    "ok" << 1));
        ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
        ASSERT_STRING_CONTAINS(status.reason(), "found non-object");
        ASSERT_STRING_CONTAINS(status.reason(), "in 'cursor.firstBatch' field");
    }

    TEST_F(FetcherTest, FirstBatchEmptyArray) {
        ASSERT_OK(fetcher->schedule());
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSONArray()) <<
                                    "ok" << 1));
        ASSERT_OK(status);
        ASSERT_TRUE(documents.empty());
    }

    TEST_F(FetcherTest, FetchOneDocument) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSON_ARRAY(doc)) <<
                                    "ok" << 1));
        ASSERT_OK(status);
        ASSERT_EQUALS(0, cursorId);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
    }

    TEST_F(FetcherTest, SetNextActionToContinueWhenNextBatchIsNotAvailable) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        callbackHook = [](const StatusWith<Fetcher::BatchData>& fetchResult,
                          Fetcher::NextAction* nextAction,
                          BSONObjBuilder* getMoreBob) {
            ASSERT_OK(fetchResult.getStatus());
            Fetcher::BatchData batchData{fetchResult.getValue()};

            ASSERT(nextAction);
            *nextAction = Fetcher::NextAction::kGetMore;
            ASSERT(getMoreBob);
            getMoreBob->append("getMore", batchData.cursorId);
            getMoreBob->append("collection", batchData.nss.coll());
        };
        processNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                     "ns" << "db.coll" <<
                                                     "firstBatch" << BSON_ARRAY(doc)) <<
                                    "ok" << 1));
        ASSERT_OK(status);
        ASSERT_EQUALS(0, cursorId);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
    }

    TEST_F(FetcherTest, FetchMultipleBatches) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc3 = BSON("_id" << 3);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 0LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc3)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc3, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
        ASSERT_FALSE(fetcher->isActive());

        ASSERT_FALSE(getNet()->hasReadyRequests());
    }

    TEST_F(FetcherTest, ScheduleGetMoreAndCancel) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        fetcher->cancel();
        finishProcessingNetworkResponse();
        ASSERT_NOT_OK(status);
    }

    TEST_F(FetcherTest, ScheduleGetMoreButShutdown) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        getExecutor().shutdown();
        finishProcessingNetworkResponse();
        ASSERT_NOT_OK(status);
    }

    void setNextActionToNoAction(const StatusWith<Fetcher::BatchData>& fetchResult,
                                 Fetcher::NextAction* nextAction,
                                 BSONObjBuilder* getMoreBob) {
        *nextAction = Fetcher::NextAction::kNoAction;
    }

    TEST_F(FetcherTest, UpdateNextActionAfterSecondBatch) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));

        callbackHook = setNextActionToNoAction;

        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc2, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kNoAction == nextAction);
        ASSERT_FALSE(fetcher->isActive());
    }

    /**
     * This will be invoked twice before the fetcher returns control to the replication executor.
     */
    void shutdownDuringSecondBatch(const StatusWith<Fetcher::BatchData>& fetchResult,
                                   Fetcher::NextAction* nextAction,
                                   BSONObjBuilder* getMoreBob,
                                   const BSONObj& doc2,
                                   ReplicationExecutor* executor, bool* isShutdownCalled) {
        if (*isShutdownCalled) {
            return;
        }

        // First time during second batch
        ASSERT_OK(fetchResult.getStatus());
        Fetcher::BatchData batchData{fetchResult.getValue()};
        ASSERT_EQUALS(1U, batchData.documents.size());
        ASSERT_EQUALS(doc2, batchData.documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == *nextAction);
        ASSERT(getMoreBob);
        getMoreBob->append("getMore", batchData.cursorId);
        getMoreBob->append("collection", batchData.nss.coll());

        executor->shutdown();
        *isShutdownCalled = true;
    }

    TEST_F(FetcherTest, ShutdownDuringSecondBatch) {
        ASSERT_OK(fetcher->schedule());
        const BSONObj doc = BSON("_id" << 1);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "firstBatch" << BSON_ARRAY(doc)) <<
                                     "ok" << 1));
        getNet()->runReadyNetworkOperations();
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, documents.size());
        ASSERT_EQUALS(doc, documents.front());
        ASSERT_TRUE(Fetcher::NextAction::kGetMore == nextAction);
        ASSERT_TRUE(fetcher->isActive());

        ASSERT_TRUE(getNet()->hasReadyRequests());
        const BSONObj doc2 = BSON("_id" << 2);
        scheduleNetworkResponse(BSON("cursor" << BSON("id" << 1LL <<
                                                      "ns" << "db.coll" <<
                                                      "nextBatch" << BSON_ARRAY(doc2)) <<
                                     "ok" << 1));

        bool isShutdownCalled = false;
        callbackHook = stdx::bind(shutdownDuringSecondBatch,
                                  stdx::placeholders::_1,
                                  stdx::placeholders::_2,
                                  stdx::placeholders::_3,
                                  doc2,
                                  &getExecutor(), &isShutdownCalled);

        getNet()->runReadyNetworkOperations();
        ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
        ASSERT_FALSE(fetcher->isActive());
    }

} // namespace
