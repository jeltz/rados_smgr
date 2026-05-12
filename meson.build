project('rados_smgr', ['c'])

fs = import('fs')

pg_config = find_program(get_option('pg_config'))

bindir = run_command(pg_config, '--bindir', check: true).stdout().strip()
includedir_server = run_command(pg_config, '--includedir-server', check: true).stdout().strip()
pkglibdir = run_command(pg_config, '--pkglibdir', check: true).stdout().strip()

librados = meson.get_compiler('c').find_library('rados')

shared_module(
    'rados_smgr',
    'src/rados_smgr.c',
    include_directories: [includedir_server],
    install: true,
    install_dir: pkglibdir,
    dependencies: [librados],
    name_prefix: '',
)

testwrap = files('testwrap')
perl = find_program('perl')
pg_regress = find_program('pg_regress', dirs: [pkglibdir / 'pgxs/src/test/regress'])

tests = ['t/basic.pl']

env = environment()
env.prepend('PATH', bindir)
env.set('PG_REGRESS', pg_regress.full_path())

foreach test : tests
    test(
        test,
        testwrap,
        kwargs: {
            'protocol': 'tap',
            'env': env,
        },
        args: [
            fs.stem(fs.name(test)),
            perl.full_path(),
            '-I', pkglibdir / 'pgxs/src/test/perl',
            files(test),
        ],
    )
endforeach
