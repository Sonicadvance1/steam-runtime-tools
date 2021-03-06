#!/usr/bin/perl

# Copyright © 2017 Collabora Ltd
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is part of libcapsule.
#
# libcapsule is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation; either version 2.1 of the
# License, or (at your option) any later version.
#
# libcapsule is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with libcapsule.  If not, see <http://www.gnu.org/licenses/>.

use autodie;
use warnings;
use strict;

use File::Temp qw();
use IPC::Run qw(run);
use Test::More;

use FindBin;
use lib $FindBin::Bin;

use CapsuleTest;

skip_all_unless_nm;

my $test_tempdir = File::Temp->newdir();
diag "Working directory: $test_tempdir";
chdir $test_tempdir;

my ($output, $ignored, $error);

my @libcapsule_environment;
if (length $ENV{CAPSULE_TESTS_UNINSTALLED}) {
    # Make sure the shim can load the just-built libcapsule
    push @libcapsule_environment, "LD_LIBRARY_PATH=$ENV{G_TEST_BUILDDIR}/.libs";
}
delete $ENV{MAKEFLAGS};

my @configure_arguments = ();

if (length $ENV{CAPSULE_TESTS_GNU_BUILD}) {
    push @configure_arguments, '--build', $ENV{CAPSULE_TESTS_GNU_BUILD};
}

if (length $ENV{CAPSULE_TESTS_GNU_HOST}) {
    push @configure_arguments, '--host', $ENV{CAPSULE_TESTS_GNU_HOST};
}

run_ok([$CAPSULE_INIT_PROJECT_TOOL,
        '--search-tree=/',
        '--runtime-tree=/run/host',
        'libz.so.1/1.2.3',
        'libxml2.so.2']);
run_ok([
        'sh', '-euc', 'cd "$1"; shift; ./configure "$@"',
        'sh', "$test_tempdir/libz-proxy",
        @configure_arguments,
        '--with-search-tree=/',
        '--with-runtime-tree=/host',
    ], '>&2');
run_ok(['make', '-C', "$test_tempdir/libz-proxy", 'V=1'], '>&2');
ok(-e "$test_tempdir/libz-proxy/libz.la");
ok(-e "$test_tempdir/libz-proxy/.libs/libz.so");
ok(-e "$test_tempdir/libz-proxy/.libs/libz.so.1");
ok(-e "$test_tempdir/libz-proxy/.libs/libz.so.1.2.3");
ok(-e "$test_tempdir/libz-proxy/libxml2.la");
ok(-e "$test_tempdir/libz-proxy/.libs/libxml2.so");
ok(-e "$test_tempdir/libz-proxy/.libs/libxml2.so.2");
ok(-e "$test_tempdir/libz-proxy/.libs/libxml2.so.2.0.0");
{
    local $/ = undef;   # read entire file in one go
    open(my $fh, '<', "$test_tempdir/libz-proxy/shim/libz.so.1.c");
    my $source = <$fh>;
    like($source, qr{\.default_prefix\s*=\s*"/host"},
        'Configure-time runtime tree takes precedence');
    unlike($source, qr{\.default_prefix\s*=\s*"/run/host"},
        'Init-time runtime tree is not used');
    unlike($source, qr{\.default_prefix\s*=\s*"/"},
        'Search tree is not used at runtime');
    close $fh;
}
{
    local $/ = undef;   # read entire file in one go
    open(my $fh, '<', "$test_tempdir/libz-proxy/shim/libxml2.so.2.c");
    my $source = <$fh>;
    like($source, qr{\.default_prefix\s*=\s*"/host"},
        'Configure-time runtime tree takes precedence');
    unlike($source, qr{\.default_prefix\s*=\s*"/run/host"},
        'Init-time runtime tree is not used');
    unlike($source, qr{\.default_prefix\s*=\s*"/"},
        'Search tree is not used at runtime');
    close $fh;
}

run_ok([$CAPSULE_SYMBOLS_TOOL, 'libz.so.1'],
    '>', \$output);
my @symbols_wanted = sort(split /\n/, $output);
foreach my $sym (@symbols_wanted) {
    diag "- $sym";
}

sub uniq {
    my @unique;
    my %seen;
    foreach my $item (@_) {
        push @unique, $item unless $seen{$item};
        $seen{$item} = 1;
    }
    return @unique;
}

my @symbols_produced =
    grep { !/^ZLIB_\d+\./ }
    get_symbols_with_nm("$test_tempdir/libz-proxy/.libs/libz.so.1");
is_deeply \@symbols_wanted, [grep {! m/^capsule_meta $/} @symbols_produced];

# Do the same thing with capsule-symbols.
# We can't load the library unless we let it load its real
# implementation, and it's hardwired to get that from /host, which
# probably doesn't exist... so cheat by overriding it.
run_ok(['env', "CAPSULE_PREFIX=/", @libcapsule_environment,
        $CAPSULE_SYMBOLS_TOOL, "$test_tempdir/libz-proxy/.libs/libz.so.1"],
    '>', \$output);
@symbols_produced = grep { !/capsule_meta\b/ } sort(split /\n/, $output);
foreach my $sym (@symbols_produced) {
    diag "- $sym";
}

if ($symbols_produced[0] eq $symbols_produced[1]) {
    TODO: {
        local $TODO = 'Each symbol shows up twice on some OSs';
        is_deeply \@symbols_wanted, \@symbols_produced;
    }
}

is_deeply \@symbols_wanted, [uniq @symbols_produced];

# Make sure the symbols list appears older than the shared list.
# Exclude one symbol, effectively behaving as though we'd used an
# older version of zlib that didn't have it.
{
    open(my $fh, '>', "$test_tempdir/libz-proxy/shim/libz.so.1.symbols");
    foreach my $symbol (@symbols_wanted) {
        print $fh "$symbol\n" unless $symbol =~ m/^zlibCompileFlags/;
    }
    close($fh);
}
run_ok(['touch', '-d', '1980-01-01 00:00',
        "$test_tempdir/libz-proxy/shim/libz.so.1.symbols"], '>&2');
# This doesn't fail or regenerate the symbols list
run_ok(['make', '-C', "$test_tempdir/libz-proxy", 'V=1'], '>&2');
{
    local $/ = undef;   # read entire file in one go
    open(my $fh, '<', "$test_tempdir/libz-proxy/shim/libz.so.1.symbols");
    my $symbols = <$fh>;
    unlike($symbols, qr{^zlibCompileFlags}m);
    close $fh;
}

# Make the shared list have actually differing contents
run_ok(['touch', '-d', '1980-01-01 00:00',
        "$test_tempdir/libz-proxy/shim/libz.so.1.symbols"], '>&2');
rename("$test_tempdir/libz-proxy/shim/libz.so.1.shared",
    "$test_tempdir/libz-proxy/shim/libz.so.1.shared.good");
{
    open(my $fh, '>', "$test_tempdir/libz-proxy/shim/libz.so.1.shared");
    print $fh "libfoo.so.2\n";
    print $fh "libbar.so.3\n";
    close($fh);
}
# This does fail
run(['make', '-C', "$test_tempdir/libz-proxy", 'V=1'],
    '>', \$ignored, '2>', \$error);
diag_multiline $error;
like($error, qr{ERROR.*make maintainer-update-capsule-symbols}s);
# ... and does not fix the problem
run(['make', '-C', "$test_tempdir/libz-proxy", 'V=1'],
    '>', \$ignored, '2>', \$error);
diag_multiline $error;
like($error, qr{ERROR.*make maintainer-update-capsule-symbols}s);

# Pretend we had listed the symbols of libfoo and libbar, and go back
# to just zlib
rename("$test_tempdir/libz-proxy/shim/libz.so.1.shared",
    "$test_tempdir/libz-proxy/shim/libz.so.1.symbols.updated-for");
rename("$test_tempdir/libz-proxy/shim/libz.so.1.shared.good",
    "$test_tempdir/libz-proxy/shim/libz.so.1.shared");
{
    open(my $fh, '>', "$test_tempdir/libz-proxy/shim/libz.so.1.symbols");
    close($fh);
}
run_ok(['make', '-C', "$test_tempdir/libz-proxy", 'V=1',
        'maintainer-update-capsule-symbols'], '>&2');
# The file has been updated with the full list of symbols
{
    local $/ = undef;   # read entire file in one go
    open(my $fh, '<', "$test_tempdir/libz-proxy/shim/libz.so.1.symbols");
    my $symbols = <$fh>;
    like($symbols, qr{^zlibCompileFlags}m);
    close $fh;
}
run_ok(['make', '-C', "$test_tempdir/libz-proxy", 'V=1'], '>&2');

@symbols_produced =
    grep { !/^ZLIB_\d+\./ }
    get_symbols_with_nm("$test_tempdir/libz-proxy/.libs/libz.so.1");
is_deeply \@symbols_wanted, [grep {! m/^capsule_meta $/} @symbols_produced];

run_ok(['env', "CAPSULE_PREFIX=/", @libcapsule_environment,
        $CAPSULE_SYMBOLS_TOOL, "$test_tempdir/libz-proxy/.libs/libz.so.1"],
    '>', \$output);
@symbols_produced = grep { !/^capsule_meta\b/ } sort(split /\n/, $output);
foreach my $sym (@symbols_produced) {
    diag "- $sym";
}

if ($symbols_produced[0] eq $symbols_produced[1]) {
    TODO: {
        local $TODO = 'Each symbol shows up twice on some OSs';
        is_deeply \@symbols_wanted, \@symbols_produced;
    }
}
is_deeply \@symbols_wanted, [uniq @symbols_produced];

chdir '/';
done_testing;

# vim:set sw=4 sts=4 et:
