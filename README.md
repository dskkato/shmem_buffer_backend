# shmem_buffer_backend

`shmem_buffer_backend` is a `rosidl::Buffer` backend that shares memory between
compatible ROS 2 endpoints on Linux and Windows. Linux uses anonymous memfd
allocations; Windows uses session-local named file mappings.

See the [user guide](docs/index.html) for an illustrated overview, setup steps,
and C++ / Python examples. The guide is a standalone site ready to be served
with GitHub Pages directly from the `docs/` directory.

## Attribution

This repository is developed and maintained outside the Open Source Robotics
Foundation (OSRF). Its implementation was developed with reference to the
OSRF [`rosidl_buffer_backends`](https://github.com/ros2/rosidl_buffer_backends)
implementation.

## Packages

- `shmem_buffer`: shared-memory allocation, pooling, handles, Linux FD
  brokering, and Windows named-mapping import.
- `shmem_buffer_backend_py`: Python zero-copy bindings for the shared-memory backend
  (installs the `shmem_buffer` Python module).
- `shmem_buffer_backend`: rosidl buffer backend plugin.
- `shmem_buffer_backend_msgs`: descriptor message used for inter-process import.

## Python zero-copy access

The `shmem_buffer_backend_py` package exposes Python buffer-protocol access
through the `shmem_buffer` Python module. NumPy is optional and consumes the
standard `memoryview` without copying:

```python
import numpy as np
from shmem_buffer import allocate_buffer, read_buffer, write_buffer

buffer = allocate_buffer(1024)
with write_buffer(buffer) as view:
    array = np.frombuffer(view, dtype=np.uint8)
    array[:] = 7
    del array  # Derived views must be released before leaving the scope.

message.data = buffer
```

```python
view = read_buffer(received_message.data)
array = np.frombuffer(view, dtype=np.uint8)
process(array)
```

Read views are read-only. Their read lease is retained by derived buffer objects
and released automatically when the last view is destroyed. Write views are
exclusive and are finalized when the scope closes; a derived write view must not
escape the scope, and closing raises `BufferError` while one remains alive.

## Benchmark

The benchmark package, benchmark results, report, and figures are maintained
in the separate [`shmem_buffer_backend_benchmark`](https://github.com/dskkato/shmem_buffer_backend_benchmark)
repository.
