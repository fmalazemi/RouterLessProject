#!/usr/bin/perl

#
# run booksim on all files in a given folder
#

use warnings;
use strict;

# settings
my $inputDir = "./examples/project_inputs";
my $outputDir = "./examples/project_outputs";

print "Starting config file generation\n";

# create file path if it doesn't exist
if (! -e $outputDir)
{
    print "Creating directory: ".$outputDir."\n";
    mkdir($outputDir) or die "Can't create file path\n";
}

opendir(my $dh, $inputDir) or die "Can't open input directory\n";
while (readdir $dh)
{
    my $inputFileName = $_;

    if ($inputFileName ne "." and $inputFileName ne "..")
    {
        print "Running file: ".$inputFileName."\n";

# set the output file name: cutoff extension and append to the file name
        my $outputFileName = $inputFileName;
        $outputFileName = $outputFileName.".log";

        system("./booksim ".$inputDir."/".$inputFileName." > ".$outputDir."/".$outputFileName);
    }
}
closedir $dh;

exit();

