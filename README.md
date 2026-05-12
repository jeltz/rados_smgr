# rados_smgr

Proof of concept PostgreSQL SMGR plugin for testing how it would be like to
implement a real new storage manager based on the pluggable SMGR patches. Uses
librados to implement a Ceph based storage manager.

## Dependencies

- PostgreSQL with the right patches
- librados
- Ceph (test dependency)

## Building

    meson setup -Dpg_config=/path/to/pg_config build
    cd build
    meson compile

## Running the tests

    ./start_ceph.sh
    cd build
    meson install && meson test --print-errorlogs
