use strict;
use warnings;
use Cwd qw(abs_path cwd);
use File::Basename qw(dirname);
use PostgreSQL::Test::Cluster;
use Test::More;

my $cwd = cwd;
my $ceph_dir = abs_path(dirname(__FILE__) . '/../ceph');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = '$cwd/rados_smgr'
allow_in_place_tablespaces = true
rados_smgr.id = 'pg'
rados_smgr.mon_host = '[v2:127.0.0.1:3300]'
rados_smgr.keyring = '$ceph_dir/keyring'
rados_smgr.pool = 'test'
});
$node->start;

is(
	$node->safe_psql('postgres', "SELECT * FROM pg_get_loaded_modules() WHERE module_name = 'rados_smgr'"),
	'rados_smgr|0.0.1|rados_smgr.so',
	'Failed to load extension',
);

$node->safe_psql('postgres', "CREATE TABLESPACE rados LOCATION ''");

$node->safe_psql('postgres', "CREATE TABLE t1 (x int, y text) TABLESPACE rados");

$node->safe_psql('postgres', "INSERT INTO t1 VALUES (1, 'foo')");

$node->restart;

is(
	$node->safe_psql('postgres', "TABLE t1"),
	'1|foo',
	'Failed to load extension',
);

$node->stop;

done_testing;
