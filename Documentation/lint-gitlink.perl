#!/usr/bin/perl

use strict;
use warnings;

# Parse arguments, a simple state machine for input like:
#
# howto/*.txt config/*.txt --section=1 git.txt git-add.txt [...] --to-lint git-add.txt a-file.txt [...]
my %TXT;
my %SECTION;
my $section;
my $lint_these = 0;
for my $arg (@ARGV) {
	if (my ($sec) = $arg =~ /^--section=(\d+)$/s) {
		$section = $sec;
		next;
	}

	my ($name) = $arg =~ /^(.*?)\.txt$/s;
	unless (defined $section) {
		$TXT{$name} = $arg;
		next;
	}

	$SECTION{$name} = $section;
}

my $exit_code = 0;
sub report {
	my ($pos, $line, $target, $msg) = @_;
	substr($line, $pos) = "' <-- HERE";
	$line =~ s/^\s+//;
	print "$ARGV:$.: error: $target: $msg, shown with 'HERE' below:\n";
	print "$ARGV:$.:\t'$line\n";
	$exit_code = 1;
}

@ARGV = sort values %TXT;
die "BUG: Nothing to process!" unless @ARGV;
while (<>) {
	my $line = $_;
	while ($line =~ m/linkgit:((.*?)\[(\d)\])/g) {
		my $pos = pos $line;
		my ($target, $page, $section) = ($1, $2, $3);

		# De-AsciiDoc
		$page =~ s/{litdd}/--/g;

		if (!exists $TXT{$page}) {
			report($pos, $line, $target, "link outside of our own docs");
			next;
		}
		if (!exists $SECTION{$page}) {
			report($pos, $line, $target, "link outside of our sectioned docs");
			next;
		}
		my $real_section = $SECTION{$page};
		if ($section != $SECTION{$page}) {
			report($pos, $line, $target, "wrong section (should be $real_section)");
			next;
		}
	}
	# this resets our $. for each file
	close ARGV if eof;
}

exit $exit_code;
