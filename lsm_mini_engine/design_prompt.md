We would like to design a log-structured merge tree (LSM) mini-engine.

Specification:
 * Write-optimized KV store
 * in-memory memtable
 * flush to sorted SSTable files 
 * simple read path that checks memtable then SSTables

Design issues to consider:
 * write amplification tradeoffs
 * file format design
 * iterator merging
