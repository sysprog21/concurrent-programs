# Concurrent Linked List With Hazard Pointer

## Runtime States In Statistics

Following are the explanation of variables, when `RUNTIME_STAT` defined :

* **retry**    is the number of retries in the __list_find function.
* **contains** is the number of wait-free contains in the __list_find function that curr pointer pointed.
* **traversal** is the number of list element traversal in the __list_find function.
* **fail**     is the number of CAS() failures.
* **del**      is the number of list_delete operation failed and restart again.
* **ins**      is the number of list_insert operation failed and restart again.
* **deletes**  is the number of linked list elements deleted.
* **inserts**  is the number of linked list elements created.
* **load**     is the number of atomic_load operation in list_delete, list_insert and __list_find.
* **store**    is the number of atomic_store operation in list_delete, list_insert and __list_find.
