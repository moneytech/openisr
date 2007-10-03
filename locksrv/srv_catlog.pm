###################################################################
# srv_catlog.pm - Append a session log to the main user log
###################################################################

#
#                       Internet Suspend/Resume
#           A system for capture and transport of PC state
#
#              Copyright (c) 2002-2004, Intel Corporation
#            Copyright (c) 2004, Carnegie Mellon University
#
# This software is distributed under the terms of the Eclipse Public
# License, Version 1.0 which can be found in the file named LICENSE.Eclipse.
# ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS SOFTWARE CONSTITUTES
# RECIPIENT'S ACCEPTANCE OF THIS AGREEMENT
#

##########
# Prologue
##########
use strict;
use File::stat;
use Socket;
use Sys::Hostname;

####################
# begin main routine
####################

# Variables
my $srcfile;
my $userid;
my $parcel;
my $userdir;
my $line;

#
# Parse the command line args
#
no strict 'vars';
getopts('hu:p:f:');

if ($opt_h) {
    usage();
}
if (!$opt_u) {
    usage("Missing userid (-u)");
}
if (!$opt_p) {
    usage("Missing parcel name (-p)");
}
if (!$opt_f) {
    usage("Missing source log file (-f)");
}
$srcfile = $opt_f;
$userid = $opt_u;
$parcel = $opt_p;
use strict 'vars';

# Set some variable names
$userdir = "$Server::CONTENT_ROOT/$userid/$parcel";

# Cat the session log to the parcel log and delete session log
open(INFILE, "$userdir/$srcfile") 
    or errexit("Unable to open session log ($srcfile) for reading");
open(OUTFILE, ">>$userdir/session.log") 
    or errexit("Unable to open user log for appending");
while ($line = <INFILE>) {
    print OUTFILE $line;
}
close(INFILE);
close(OUTFILEFILE);
unlink("$userdir/$srcfile");
exit 0;



############################################
# usage - print help message and terminate #
############################################

sub usage
{
    my $msg = shift;
    my $progname;

    # Strip any path information from the program name
    ($progname = $0) =~ s#.*/##s; 
    
    if ($msg) {
        print "$progname: $msg\n\n";
    }
    
    print "Usage: $progname [-h] -u <userid> -p <parcel> -f <file>\n";
    print "Options:\n";
    print "  -h           Print this message\n";
    print "  -f <file>    Session log to append to parcel log\n";
    print "  -u <userid>  User ID for this parcel\n";
    print "  -p <parcel>  Parcel name\n";
    print "\n";
    exit 0;
}
