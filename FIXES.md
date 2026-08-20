# Secure Fix - Addresses all critical issues from code review

## Fixed:

1. Binary-safe Redis: SETEX now uses %b with explicit length, SADD uses redisAppendCommandArgv with argv array, PUBLISH uses %b. No string concatenation that breaks on spaces/quotes.

2. Shell injection removed: Replaced system(curl ...) with libcurl HttpWorker background thread. No shell, no escaping, bounded queue (256). Uses CURLOPT_POSTFIELDS with size.

3. Reply handling robust: Counts expectedReplies, checks REDIS_OK, checks null, checks REDIS_REPLY_ERROR, freeReplyObject for every reply. On error, marks redis err=1 for reconnect.

4. RemovePlayer correct map: Tracks _lastMapForChar in QueueUpdate, publishes removal to map:lastMap and map:0. No more stale markers.

5. Throttling typo fixed: m[guidLow] = now; (was truncated in review)

6. Redis reconnect: TryRedisReconnect with 5s backoff, redisConnectWithTimeout, timeout config LiveMap.RedisTimeoutMs.

7. Memory leaks fixed: Every redisReply freed, curl_slist freed, curl_easy_cleanup.

8. Concurrency: Split locks - _lock for queue+lastMap, _redisLock for redis. Flush swaps queue under _lock then releases before network IO. No holding _lock during redisAppendCommand/redisGetReply.

9. Queue overflow logging, dropped counter, configurable QueueMax.

CMakeLists.txt needs: find libhiredis and libcurl, link curl

add: target_link_libraries(mod_live_map PRIVATE hiredis curl)
