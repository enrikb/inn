##  $Id$
##
##  ihave control message handler.
##
##  Copyright 2001 by Marco d'Itri <md@linux.it>
##
##  Redistribution and use in source and binary forms, with or without
##  modification, are permitted provided that the following conditions
##  are met:
##
##   1. Redistributions of source code must retain the above copyright
##      notice, this list of conditions and the following disclaimer.
##
##   2. Redistributions in binary form must reproduce the above copyright
##      notice, this list of conditions and the following disclaimer in the
##      documentation and/or other materials provided with the distribution.

use strict;

sub control_ihave {
    my ($par, $sender, $replyto, $site, $action, $log, $approved,
        $headers, $body) = @_;

    if ($action eq 'mail') {
        my $mail = sendmail("ihave by $sender");
        print $mail map { s/^~/~~/; "$_\n" } @$body;
        close $mail or logdie('Cannot send mail: ' . $!);
    } elsif ($action eq 'log') {
        if ($log) {
            logger($log, "ihave $sender", $headers, $body);
        } else {
            logmsg("ihave $sender");
        }
    } elsif ($action eq 'doit') {
        my $tempfile = "$INN::Config::tmpdir/ihave.$$";
        open(GREPHIST, "| $INN::Config::newsbin/grephistory -i > $tempfile")
            or logdie('Cannot run grephistory: ' . $!);
	foreach (@$body) {
            print GREPHIST "$_\n";
        }
        close GREPHIST;

        if (-s $tempfile) {
            my $inews = open("$INN::Config::inews -h")
                or logdie('Cannot run inews: ' . $!);
            print $inews "Newsgroups: to.$site\n"
               . "Subject: cmsg sendme $INN::Config::pathhost\n"
               . "Control: sendme $INN::Config::pathhost\n\n";
            open(TEMPFILE, $tempfile) or logdie("Cannot open $tempfile: $!");
            print $inews $_ while <TEMPFILE>;
            close $inews or die $!;
            close TEMPFILE;
        }
        unlink $tempfile;
    }
}

1;
