#!/usr/bin/bash

set -e

CEPH_DIR=${CEPH_DIR:-./ceph}
CEPH_V2_PORT=${CEPH_V2_PORT:-3300}

pkill ceph-mon || :
pkill ceph-osd || :
rm -rf "$CEPH_DIR"

mkdir "$CEPH_DIR"
tee "$CEPH_DIR/ceph.conf" <<EOS >/dev/null
[global]
fsid = $(uuidgen)
admin_socket = $CEPH_DIR/\$name.asok
log_file = $CEPH_DIR/\$name.log
mon_host = [v2:127.0.0.1:$CEPH_V2_PORT]
osd_pool_default_size = 1

[mon]
mon_initial_members = a
mon_data = $CEPH_DIR/\$name
mon_cluster_log_file = $CEPH_DIR/cluster.\$name.log
auth_allow_insecure_global_id_reclaim = false

[osd]
osd_data = $CEPH_DIR/\$name

bluestore_block_create = true
bluestore_block_db_create = true
bluestore_block_wal_create = true

[client]
keyring = $CEPH_DIR/keyring
EOS

ceph-authtool --create-keyring "$CEPH_DIR/keyring"
ceph-authtool --gen-key --name=mon. --cap mon 'allow *' "$CEPH_DIR/keyring"
ceph-authtool --gen-key --name=osd.1 --cap mon 'allow *' "$CEPH_DIR/keyring"
ceph-authtool --gen-key --name=client.admin --cap mon 'allow *' --cap osd 'allow *' "$CEPH_DIR/keyring"
ceph-authtool --gen-key --name=client.pg --cap mon 'allow r' --cap osd 'allow rwx' "$CEPH_DIR/keyring"

ceph-mon -i a --mkfs -c "$CEPH_DIR/ceph.conf" --keyring="$CEPH_DIR/keyring"
ceph-mon -i a -c "$CEPH_DIR/ceph.conf"

mkdir "$CEPH_DIR/osd.1"
ceph-osd -i 1 --mkfs -c "$CEPH_DIR/ceph.conf" --keyring="$CEPH_DIR/keyring"
ceph-osd -i 1 -c "$CEPH_DIR/ceph.conf" --keyring="$CEPH_DIR/keyring"
ceph -c "$CEPH_DIR/ceph.conf" osd pool create test
