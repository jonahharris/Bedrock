#include "../BedrockClusterTester.h"

struct l_GracefulFailoverTest : tpunit::TestFixture {
    l_GracefulFailoverTest()
        : tpunit::TestFixture("l_GracefulFailover",
                              TEST(l_GracefulFailoverTest::test)
                             ) { }

    BedrockClusterTester* tester;

    void test()
    {
        // Verify the existing master is up.
        BedrockClusterTester* tester = BedrockClusterTester::testers.front();
        BedrockTester* master = tester->getBedrockTester(0);

        int count = 0;
        bool success = false;
        while (count++ < 50) {
            SData cmd("Status");
            string response = master->executeWaitVerifyContent(cmd);
            STable json = SParseJSONObject(response);
            if (json["state"] == "MASTERING") {
                success = true;
                break;
            }

            // Give it another second...
            sleep(1);
        }

        cout << "======================================== node 0 is mastering." << endl;

        // So here, we start with node 0 mastering.
        // We then spin up a few threads that continually spam all three nodes with both read and write commands.
        // These should complete locally or escalate to master as appropriate.
        // We then shut down node 0. Node 1 will take over as master.
        // All commands sent to Node 0 after this point should result in a "connection refused" error. This is fine.
        // We verify that node 1 comes up as master.
        // We then continue spamming for a few seconds and make sure every command returns either success, or
        // connection refused.
        // Then we bring Node 0 back up, and verify that it takes over as master. Send a few more commands.
        //
        // We should have sent hundreds of commands, and they all should have either succeeded, or been "connection
        // refused".
        //
        // Thus concludes our test:
        // TODO:
        // https commands.
        // Commands scheduled in the future, or waiting on future commits.
        //

        // Step 1: everything is already up and running. Let's start spamming.
        list<thread> threads;
        atomic<bool> done;
        done.store(false);
        mutex m;
        vector<list<SData>> allresults(60);

        atomic<int> commandID(10000);

        map<string, int> counts;

        // Ok, start up 60 clients.
        for (int i = 0; i < 60; i++) {
            // Start a thread.
            threads.emplace_back([tester, i, &m, &done, &allresults, &counts, &commandID]() {
                int currentNodeIndex = i % 3;
                while(!done.load()) {
                    // Send some read or some write commands.
                    vector<SData> requests;
                    size_t numCommands = 50;
                    for (size_t j = 0; j < numCommands; j++) {
                        string randCommand = " r_" + to_string(commandID.fetch_add(1)) + "_r";
                        // Every 10th client makes HTTPS requests (1/5th as many, cause they take forever).
                        if (i % 10 == 0) {
                            if (j % 5 == 0) {
                                SData query("sendrequest" + randCommand);
                                query["writeConsistency"] = "ASYNC";
                                query["senttonode"] = to_string(currentNodeIndex);
                                query["clientID"] = to_string(i);
                                query["response"] = "756";
                                requests.push_back(query);
                            }
                        } else if (i % 2 == 0) {
                            // Every remaining even client makes write requests.
                            SData query("idcollision" + randCommand);
                            query["writeConsistency"] = "ASYNC";
                            query["peekSleep"] = "5";
                            query["processSleep"] = "5";
                            query["response"] = "756";
                            query["senttonode"] = to_string(currentNodeIndex);
                            query["clientID"] = to_string(i);
                            requests.push_back(query);
                        } else {
                            // Any other client makes read requests.
                            SData query("testcommand" + randCommand);
                            query["peekSleep"] = "10";
                            query["response"] = "756";
                            query["senttonode"] = to_string(currentNodeIndex);
                            query["clientID"] = to_string(i);
                            requests.push_back(query);
                        }
                    }

                    // Ok, send them all!
                    cout << "Client " << i << " sending " << requests.size() << " to node " << currentNodeIndex << endl;
                    BedrockTester* node = tester->getBedrockTester(currentNodeIndex);
                    auto results = node->executeWaitMultipleData(requests, 1, false, true);
                    size_t completed = 0;
                    for (auto& r : results) {
                        lock_guard<mutex> lock(m);
                        if (r.methodLine != "002 Socket Failed") {
                            if (r.methodLine != "756") {
                                cout << "Client "<< i << " expected 756, got: '" << r.methodLine <<  "', had completed: " << completed << endl;
                            }
                            if (counts.find(r.methodLine) != counts.end()) {
                                counts[r.methodLine]++;
                            } else {
                                counts[r.methodLine] = 1;
                            }
                            completed++;
                        } else {
                            // Got a disconnection. try on the next node.
                            break;
                        }
                    }
                    cout << "Completed " << completed << " commands of "  << requests.size() << " on client " << i << " and node " << currentNodeIndex << endl;
                    currentNodeIndex++;
                    currentNodeIndex %= 3;
                }
            });
        }

        // Let the clients get started.
        sleep(2);

        // Now our clients are spamming all our nodes. Shut down master.
        cout << "======================================== node 0 is stopping." << endl;
        tester->stopNode(0);

        // Wait for node 1 to be master.
        BedrockTester* newMaster = tester->getBedrockTester(1);
        count = 0;
        success = false;
        while (count++ < 50) {
            SData cmd("Status");
            string response = newMaster->executeWaitVerifyContent(cmd);
            STable json = SParseJSONObject(response);
            if (json["state"] == "MASTERING") {
                success = true;
                break;
            }

            // Give it another second...
            sleep(1);
        }

        // make sure it actually succeeded.
        ASSERT_TRUE(success);
        cout << "======================================== node 1 is mastering." << endl;

        // Let the spammers spam.
        sleep(3);

        // Bring master back up.
        cout << "======================================== node 0 is starting." << endl;
        tester->startNode(0);

        count = 0;
        success = false;
        while (count++ < 50) {
            SData cmd("Status");
            string response = master->executeWaitVerifyContent(cmd);
            STable json = SParseJSONObject(response);
            if (json["state"] == "MASTERING") {
                success = true;
                break;
            }

            // Give it another second...
            sleep(1);
        }

        cout << "======================================== node 0 is mastering." << endl;

        // make sure it actually succeeded.
        ASSERT_TRUE(success);

        // Great, it came back up.
        done.store(true);

        int i = 0;
        for (auto& t : threads) {
            cout << "joining " << (i++) << endl;
            t.join();
            // TODO: Verify the results of our spamming.
        }

        for (auto& p : counts) {
            cout << "method: " << p.first << ", count: " << p.second << endl;
        }
        ASSERT_EQUAL(counts.size(), 1);
    }

    // At this point, let's kill -9 the master and see if the slave takes over gracefully. We can't expect that no
    // commands fail in this case.
    //
    // Other things to check: Do timeouts on https requests actually work?
    //
    // The following cases have code added (but have not been tested) to support them:
    // We should discard commands scheduled for future execution when shutting down (but not when standing down).
    // Commands dependent on a future commit are weird. These should only be able to exist while slaving. What do we do
    // if we shut down? Requeue them? Probably.
    //
    // It would be nice to set up clustertest to run in parallel, which should be possible now.

} __l_GracefulFailoverTest;