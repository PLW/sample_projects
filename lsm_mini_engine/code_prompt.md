In the same context, we would like to sketch out C++ code for this design.
Module structure:
* wal/
  * append
  * recovery
* memtable/
  * mutable
  * frozen
* sstable/
  * builder   (write blocks, index, meta, footer)
  * reader    (seek, iterate)
  * bloom
* version/
  * manifest  (atomic publish of current file set)
* iter/
  * merging iterator
  * internal key comparator
* compaction/ 

Sketch concrete structs and APIs (e.g., SSTableBuilder, TableReader::NewIterator(), VersionSet), plus a small binary encoding spec (varints, footer, restart arrays) that’s easy to implement and test
