# Basic Specification Questions

Designing a large-scale, sharded, and replicated system requires clarifying the
expected scale, data access patterns, and failure tolerance. The most important
basic questions to ask, based on system design principles, include:

## Requirements & Scale (Data & Traffic) 

* What is the total data volume, and what is the expected daily growth rate? (Determines total storage capacity and number of shards). 
* What is the peak Request Per Second (RPS) for reads and writes? (Determines throughput requirements and caching needs). 
* What is the read-to-write ratio? (Determines if you need a read-heavy optimization or a write-heavy design). 
* What is the latency requirement? (e.g., &lt;10ms for 99% of requests).

## Sharding (Data Distribution) 

* What is the sharding key, and how will we avoid hot spots? (Choosing a key that distributes data evenly is crucial). 
* Do we need to perform range queries or multi-key joins? (Determines if using hash-based sharding or range-based sharding). 
* How will we handle rebalancing shards as data grows? (Determines the need for consistent hashing to minimize data movement).

## Replication & Consistency (Data Integrity) 

* What is the required consistency model? (Strong consistency for finance, or eventual consistency for high availability). 
* How many replicas are needed for data durability and fault tolerance? (Usually a minimum of 3 replicas per shard). 
* What is the replication mechanism (synchronous vs. asynchronous)? (Synchronous offers better consistency but higher write latency, asynchronous offers better performance).

## Cache & Indexing 

* What is the caching strategy (write-through, write-back, or cache-aside)? (Determines how the cache stays in sync with storage). 
* How do we handle cache eviction and expiration (TTL)? (Ensures stale data doesn't persist). 
* What is the indexing strategy for locating data across shards? (Essential for fast lookups in distributed systems).

## Failure Handling & Operations 

* What happens if a primary node fails? (Determines the speed of leader election and failover mechanisms). 
* How does the system detect and resolve network partitions (split-brain)? (Crucial for CAP theorem trade-offs). 
* What is the data backup and disaster recovery strategy?.

## Summary of Key Trade-offs 

* Consistency vs. Availability: (CAP Theorem) 
* Latency vs. Durability: (Synchronous vs. Asynchronous Replication) 
* Complexity vs. Performance: (Sharding adds complexity but increases throughput)

# Bibliography

[1] https://www.tencentcloud.com/techpedia/121839
[2] https://www.designgurus.io/answers/detail/most-asked-system-design-questions
[3] https://www.systemdesignhandbook.com/guides/system-design-fundamentals/
[4] https://medium.com/@poojaauma/designing-scalable-systems-10-proven-strategies-from-faang-engineering-7e7743c1f1a8
[5] https://www.alooba.com/skills/concepts/devops/distributed-systems/
[6] https://www.tryexponent.com/blog/system-design-interview-guide
[7] https://proxysql.com/blog/database-sharding/
[8] https://www.hellointerview.com/learn/system-design/problem-breakdowns/distributed-cache
[9] https://bloomberg.github.io/blazingmq/docs/architecture/clustering/
[10] https://www.dragonflydb.io/guides/azure-cache-best-practices
[11] https://www.hellointerview.com/learn/system-design/core-concepts/sharding
[12] https://www.geeksforgeeks.org/system-design/design-distributed-cache-system-design/
[13] https://blog.bytebytego.com/p/must-know-message-broker-patterns-4c4
[14] https://blog.devops.dev/understanding-replication-and-sharding-in-system-design-9453372032ca
[15] https://github.com/Devinterview-io/caching-interview-questions
[16] https://oneuptime.com/blog/post/2026-01-30-distributed-cache-design/view
[17] https://medium.com/software-engineering-interview-essentials/15-distributed-systems-interview-questions-with-clear-answers-aaabf6e7d870
[18] https://www.openlogic.com/blog/kafka-cluster-configuration-strategies
[19] https://www.designgurus.io/answers/detail/how-does-caching-work-and-what-are-common-caching-strategies-in-system-design
[20] https://www.linkedin.com/posts/arpitbhayani_how-do-indexes-work-when-your-database-is-activity-7371173649577336832-lvS0
[21] https://leetcode.com/discuss/interview-question/3671816/top-system-design-interview-questions-basic-to-high-level-questions/
[22] https://courses.cs.washington.edu/courses/cse452/22wi/lecture/L5/
[23] https://www.javacodegeeks.com/2023/10/the-split-brain-phenomenon-a-distributed-systems-dilemma.html
[24] https://profitpt.com/2021/02/16/3-questions-to-ask-when-sharding-a-database/
[25] https://www.mongodb.com/resources/products/capabilities/database-sharding-explained

# Appendix A

The CAP theorem states that a distributed database system can only provide two of three guarantees—Consistency, Availability, and Partition Tolerance—when a network failure (partition) occurs. It forces a trade-off between strict data accuracy (C) and constant accessibility (A) during network issues.

##The Three Components (CAP) 

* Consistency (C): Every read receives the most recent write or an error. All nodes see the same data at the same time. 
* Availability (A): Every request receives a response, even if it is not the most recent data. The system remains operational. 
* Partition Tolerance (P): The system continues to operate despite network failures (partitions) that break communication between nodes.

Because network partitions are inevitable, systems must choose between C and A: 

* CP (Consistency + Partition Tolerance): Prioritizes data consistency over availability. If a network partition occurs, the system will return an error or time out rather than risk returning stale data. (e.g., Banking systems, SQL databases). 
* AP (Availability + Partition Tolerance): Prioritizes availability. If a network partition occurs, all nodes remain accessible but may return different or stale data. (e.g., Social media, Cassandra, DynamoDB).

The CAP theorem is often used in system design to choose the right database, with modern systems offering configurable options.


# Appendix B

## Write-Through Cache
Data is written to the cache and the backing store simultaneously. 

How it works: When data is updated, it goes to the cache first, then immediately to the database.

Pros: High data consistency (cache and database are always in sync) and data safety.

Cons: Higher write latency because every write must wait for the database, which is slower.

Use Case: Critical data where data loss is not acceptable (e.g., financial transactions). 

## Write-Back Cache (Write-Behind)
Data is written only to the cache, and the database is updated later. 

How it works: The write is confirmed as soon as it is in the cache. The modified data (marked as "dirty") is written to the database only when the cache line is evicted or at specific intervals.

Pros: Very high write performance (fast, asynchronous).

Cons: Risk of data loss if the system crashes before the cache is flushed to the database.

Use Case: Write-heavy environments, such as gaming, user session management, or logging, where performance is prioritized. 

## Cache-Aside (Lazy Loading)
The application directly handles loading data from the database into the cache. 

How it works: The application checks the cache first. If the data isn't there (miss), it fetches it from the database, populates the cache, and returns it. For writes, the application updates the database directly and invalidates the cache.

Pros: Resilient to cache failures (if the cache goes down, the app still works).
Cons: Cache miss penalty (latency) on first read. Risk of stale data if not handled properly.

Use Case: General-purpose caching, reading, and scenarios where data updates are rare, such as web content. 

| Feature | Cache-Aside (Lazy) | Write-Through | Write-Back (Behind) |
| :------ | :----------------- | :------------ | :------------------ |
| Write Handling | App writes to DB, invalidates cache | App writes to cache & DB simultaneously  | App writes to cache, async to DB later |
| Read Handling | Lazy (on demand) | Proactive |Proactive |
| Data Consistency | Eventual | Strong | Eventual |
| Write Performance | Low | Low | High |
| Data Safety | High | High | Low (risk of loss) |

## Key Takeaways
* Write-Through is for consistency (safety).
* Write-Back is for speed (performance).
* Cache-Aside is the "default" pattern for most applications, offering flexibility and resilience.
