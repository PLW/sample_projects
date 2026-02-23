We would like to design from scratch a vectordb system using C++. Specifications: 

* the system runs on a single CPU processing node
* the vector store is managed as a set of `segments` which are comprised of a packed subset of embedding vectors (points) and the associated HNSM index
* text blocks are loaded into an in-memory segment which is flushed to a persistent segment when the vector count exceeds a configured threshold
* persistent segments are periodically merged by background threads when a segment count threshold is reached.  Smaller segments are merged to create a new larger segment, the list of `in play` segments is update by adding the new segment and deleting the merge inputs
* queries run against a snapshot of the vectordb and are not affected by either the memory segment flush or the background merges
* text blocks are parsed into tokens, individual token embedding vectors are looked up in a given (configured) HuggingFace embedding vector set, and then aggregated to form the embedding vector for the input block, the HNSW index is then incrementally updated
* HNSW indexes are merged using the algorithm defined in the paper: https://www.elastic.co/search-labs/blog/hnsw-graphs-speed-up-merging
