#!/usr/bin/env perl
# Git clean filter for Papyrus .pex files.
#
# The Papyrus compiler stamps a 64-bit compilation timestamp into the PEX
# header at byte offset 8 (right after magic[4] + major[1] + minor[1] +
# gameID[2]). That field changes on every build, so an otherwise-identical
# recompile always shows up as a git modification.
#
# This clean filter zeroes those 8 bytes so the blob git stores is stable
# across no-op rebuilds. It runs on `git add`/status comparison only; the
# real file on disk (and thus the deployed artifact) keeps its true
# timestamp. The Papyrus VM does not use this field at load time, so a
# checked-out copy with a zeroed timestamp loads and runs identically.
#
# Header layout (little-endian):
#   0x00  uint32  magic   0xFA57C0DE
#   0x04  uint8   majorVersion
#   0x05  uint8   minorVersion
#   0x06  uint16  gameID
#   0x08  uint64  compilationTime   <-- normalized to 0 here
#   ...   strings (sourceFileName, username, machineName, ...)

use strict;
use warnings;

binmode STDIN;
binmode STDOUT;

local $/;
my $data = <STDIN>;
$data = '' unless defined $data;

# Only touch genuine PEX files: magic 0xFA57C0DE stored little-endian.
if (length($data) >= 16 && substr($data, 0, 4) eq "\xDE\xC0\x57\xFA") {
    substr($data, 8, 8) = "\x00" x 8;
}

print $data;
