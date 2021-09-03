# hashmap: A concurrent hashmap implementation

Internal state is managed by the hashmap to populate buckets with linked lists.
`hashmap_keyval` nodes are allocated as needed; however, free'ing of those
structs can not be done immediately during a `hashmap_del` call. Other threads
may be concurrently using the `hashmap_keyval`.

`hashmap_del` may return a value. It will only do it once per delete, and the
calling thread must buffer the pointer for appropriate later cleanup. It can
not immediatley be free'd because other threads may also be using the same
pointer.
