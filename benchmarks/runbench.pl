#!/usr/bin/perl

use strict;

# Check for correct usage
if (@ARGV != 3) {
  print "usage: runbench.pl <dir> <name> <iters>\n";
  print "    where <dir> is the directory containing the test executable and Results subdir,\n";
  print "    <name> is the base name of the test executable, and\n";
  print "    <iters> is the number of trials to perform.\n";
  die;
}

my $dir = $ARGV[0];
my $benchname = $ARGV[1];
my $iters = $ARGV[2];
my $nthread = 8;

#If dir is not an absolute path, and doesn't start with "." already, add the "./"
if (!($dir =~ /^\// || $dir =~ /^\./)) {
    $dir = "./" . $dir;
}

print "=== dir = $dir\n";
print "=== iters = $iters\n";

#Ensure existence of $dir/Results
if (!-e "$dir/Results") {
    mkdir "$dir/Results", 0755
	or die "Cannot make $dir/Results: $!";
}

# Initialize list of allocators to test.
# uncomment the line corresponding to the allocators you want to run.
#my @alloclist = ("a2alloc");
#my @alloclist = ("libc", "kheap");
my @alloclist = ("libc", "kheap", "a2alloc");
my $allocator;

#Initialize from config file
#Each benchmark dir must contain a config.pl file that sets 
# maxtime and args for that benchmark. 
my %config;

unless (%config = do "$dir/config.pl") {
            warn "couldn't parse $dir/config.pl: $@" if $@;
            warn "couldn't do $dir/config.pl: $!"    unless %config;
            warn "couldn't run $dir/config.pl"       unless %config;
}

foreach $allocator (@alloclist) {
    print "allocator name = $allocator\n";
    # Create subdirectory for current allocator results
    if (!-e "$dir/Results/$allocator") {
	mkdir "$dir/Results/$allocator", 0755
	    or die "Cannot make $dir/Results/$allocator: $!";
    }

    # Run tests for 1 to $nthread thread
    for (my $i = 1; $i <= $nthread; $i++) {
	print "Thread $i\n";
	my $cmd1 = "echo \"\" > $dir/Results/$allocator/$benchname-$i";
	system "$cmd1";
	for (my $j = 1; $j <= $iters; $j++) {
	    my $killed = 0;
	    my $pid;
	    print "Iteration $j, maxtime $config{maxtime}\n";
	    my $cmd = "$dir/$benchname-$allocator $i $config{args} >> $dir/Results/$allocator/$benchname-$i 2>&1";

	    # Give each individual test a time limit, so we can move on to
	    # next test if one of them deadlocks.
	    alarm $config{maxtime};
	    if (!($pid = fork)) {
		#child		
		print "$cmd\n";		
		exec($cmd);
	    }
	    # on alarm, ignore SIGHUP locally so we don't kill daemon,
	    # and send SIGHUP to all child processes in same process group
	    $SIG{ALRM} = sub { 
		local $SIG{HUP} = 'IGNORE'; 
		kill("HUP", -$$); 
		$killed = 1; 
	    };
	    waitpid $pid, 0;
	    alarm 0;
	    if ($killed) { 
		print "KILLED ";
		system("echo 'Killed.' >> $dir/Results/$allocator/$benchname-$i");
		wait;
	    }

	}
    }
}


