#!/usr/bin/perl

use strict;

# Check for correct usage
if (@ARGV != 1) {
    print "usage: runall.pl <dir>\n";
    print "    where <dir> is the directory containing the benchmark subdirectories\n";
    die;
}

my $dir=$ARGV[0];
my $name;
my $iters = 5;

my @namelist = ("cache-scratch", "cache-thrash", "threadtest", "larson", "linux-scalability", "phong");
 
foreach $name ( @namelist ) {
  print "benchmark name = $name\n";
  print "running runbench.pl $dir/$name $name $iters\n";
  system "$dir/runbench.pl $dir/$name $name $iters";
  system "$dir/graphbench.pl $dir/$name $name"; 
}

