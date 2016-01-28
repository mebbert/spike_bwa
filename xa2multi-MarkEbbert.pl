#!/usr/bin/perl -w

use strict;
use warnings;

while (<>) {
	if (/\tXA:Z:(\S+)/) {
		my $l = $1;
		print;
		my @t = split("\t");
		while ($l =~ /([^,;]+),([-+]\d+),([^,]+),(\d+);/g) {
			my $mchr = ($t[6] eq $1)? '=' : $t[6]; # FIXME: TLEN/ISIZE is not calculated!
			my $seq = $t[9];
			my $phred = $t[10];
			# if alternative alignment has other orientation than primary, 
			# then print the reverse (complement) of sequence and phred string
			if ((($t[1]&0x10)>0) xor ($2<0)) {
				$seq = reverse $seq;
				$seq =~ tr/ACGTacgt/TGCAtgca/;
				$phred = reverse $phred;
			}

			# I (Mark Ebbert) am modifying to change all soft-clipped bases in the 'XA:Z' field to be hard clipped because
			# samtools is complaining that the cigar and sequence length do not match
			my $ref = $1;
			my $pos = $2;
			my $cigar = $3;
			my $oldcigar = $3;
			my $nm = $4;

			if ($cigar =~ m/S/){

				$cigar =~ s/S/H/g;

				#print "old: $oldcigar\n";
				#print "new: $cigar\n\n";
			}


			print(join("\t", $t[0], 0x100|($t[1]&0x6e9)|($pos<0?0x10:0), $ref, abs($pos), 0, $cigar, @t[6..7], 0, $seq, $phred, "NM:i:$nm"), "\n");
		}
	} else { print; }
}
