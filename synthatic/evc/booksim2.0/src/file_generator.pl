#!/usr/bin/perl

#
# config file generator for booksim
#

use warnings;
use strict;

# settings
my $booksimFilePath = ".";
my $outputFilePath = "./examples/project_inputs";
my @routerType = ('iqevc');
my @trafficPatterns = ('uniform', 'bitcomp', 'transpose');
my $classPriorities = "{0,1}";
my $class0InjectionRateStart = 0.05;
my $class0InjectionRateEnd = 0.4;
my $class1InjectionRateStart = 0.05;
my $class1InjectionRateEnd = 0.05;
my $injectionRateIncrement = 0.02;
my $allocator = "select";
my $nextroute = 0;

sub printToFile
{
    my $FILE = shift;
    my $rt = shift;
    my $tp = shift;
    my $c0ir = shift;
    my $c1ir = shift;
    my $cp = shift;
    my $alloc = shift;
    my $nr = shift;

    print $FILE "//
// EVC Router Config File -         
//
// Jason Deng, Mike Wang        
//        

//
// Flow control
//

num_vcs     = 5;
vc_buf_size = 6;

wait_for_tail_credit = 0;

vc_allocator = ".$alloc.";
sw_allocator = ".$alloc.";
alloc_iters  = 2;

credit_delay   = 2;
routing_delay  = 1;
vc_alloc_delay = 1;
sw_alloc_delay = 1;
st_final_delay = 1;

input_speedup     = 1;
output_speedup    = 1;
internal_speedup  = 1.0;

//
// Traffic
//

sim_type = latency;

warmup_periods = 3;
sample_period  = 1000;
sim_count = 1;

//
//topoogy
//

topology = mesh;
k  = 8;
n  = 2;

//
// Routing
//

router = ".$rt.";
routing_function = dor;

packet_size = 2;
use_read_write = 0;

traffic  = ".$tp.";
injection_rate = {".$c0ir.",".$c1ir."};
injection_rate_uses_flits = 1;

classes = 2;
class_priority = ".$cp.";
priority = class;

noq = 0;
hold_switch_for_packet = 0;
vc_prioritize_empty = 0;

//
// express virtual channel settings
//

evc = 1;
num_evcs = 1;
evc_prioritized_class = 1;
selective_vc_request = 1;
hold_switch_for_evc_packet = 1;
evc_next_route = ".$nr.";
";
}

# 
# generate config file for booksim
#	

print "Starting config file generation\n";

# create file path if it doesn't exist
if (! -e $outputFilePath) 
{
    print "Creating directory: ".$outputFilePath."\n";
    mkdir($outputFilePath) or die "Can't create file path\n";
}

# traffic pattern
foreach (@routerType)
{
    my $rt = $_;

    foreach (@trafficPatterns)
    {
        my $tp = $_;

# class 0 injection rate
        for (my $c0ir = $class0InjectionRateStart; $c0ir <= $class0InjectionRateEnd; $c0ir += $injectionRateIncrement)
        {
# class 1 injection rate
            for (my $c1ir = $class1InjectionRateStart; $c1ir <= $class1InjectionRateEnd; $c1ir += $injectionRateIncrement)
            {
# setup output file name
                my $outputFileName = "config-".$rt."_tp-".$tp."_c1ir-".$c1ir."_c0ir-".$c0ir."_prio-".$allocator;

                print "Creating file: ".$outputFileName."\n";
                open(OUTPUTFILE, '>'.$outputFilePath."/".$outputFileName);
                printToFile(\*OUTPUTFILE, $rt, $tp, $c0ir, $c1ir, $classPriorities, $allocator, $nextroute);
                close(OUTPUTFILE);
            }		
        }
    }
}

print "Done\n";

exit();
