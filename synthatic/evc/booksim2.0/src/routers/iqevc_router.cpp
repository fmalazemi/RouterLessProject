// $Id: iq_router.cpp 5188 2012-08-30 00:31:31Z dub $

/*
   Copyright (c) 2007-2012, Trustees of The Leland Stanford Junior University
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice, this 
   list of conditions and the following disclaimer.
   Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
   ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "iqevc_router.hpp"

#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cassert>
#include <limits>

#include "globals.hpp"
#include "random_utils.hpp"
#include "vc.hpp"
#include "routefunc.hpp"
#include "outputset.hpp"
#include "buffer.hpp"
#include "buffer_state.hpp"
#include "roundrobin_arb.hpp"
#include "allocator.hpp"
#include "switch_monitor.hpp"
#include "buffer_monitor.hpp"

IQEVCRouter::IQEVCRouter( Configuration const & config, Module *parent, 
        string const & name, int id, int inputs, int outputs )
: Router( config, parent, name, id, inputs, outputs ), _active(false)
{
    //printf("Creating router\n");

    _vcs = config.GetInt( "num_vcs" );

    // 1 express virtual channel for high priority packets
    _evc = (config.GetInt("evc") > 0);
    // number of EVCs
    _num_evcs = config.GetInt("num_evcs");
    if (_num_evcs < 1)
        Error("Must have at least 1 EVCs.");
    // enables prioritization of certain VCs based on VC occupancy
    _selective_vc_request = (config.GetInt("selective_vc_request") > 0);
    // enable HOL blocking reduction by using a future/next outputport prediction
    _evc_next_route = (config.GetInt("evc_next_route") > 0);
    // specifies the high priority packet class to use express virtual channel
    _evc_prioritized_class = config.GetInt("evc_prioritized_class");
    // hold crossbar switch configuration for high priority packets
    _hold_switch_for_evc_packet = config.GetInt("hold_switch_for_evc_packet");

    //jason: make sure there are enough VCs in total to implement the EVCs
    if (_vcs <= _num_evcs) {
        Error("Not enough VCs to implement EVCs. Must satisfy num_vcs > num_evcs");
    }

    _classes = config.GetInt("classes");

    _vc_busy_when_full = (config.GetInt("vc_busy_when_full") > 0);
    _vc_prioritize_empty = (config.GetInt("vc_prioritize_empty") > 0);

    _speculative = (config.GetInt("speculative") > 0);
    _spec_check_elig = (config.GetInt("spec_check_elig") > 0);
    _spec_check_cred = (config.GetInt("spec_check_cred") > 0);
    _spec_mask_by_reqs = (config.GetInt("spec_mask_by_reqs") > 0);

    _routing_delay    = config.GetInt( "routing_delay" );
    _vc_alloc_delay   = config.GetInt( "vc_alloc_delay" );
    if(!_vc_alloc_delay) {
        Error("VC allocator cannot have zero delay.");
    }
    _sw_alloc_delay   = config.GetInt( "sw_alloc_delay" );
    if(!_sw_alloc_delay) {
        Error("Switch allocator cannot have zero delay.");
    }

    // Routing
    string const rf = config.GetStr("routing_function") + "_" + config.GetStr("topology");

    //jason: set the routing function and topology
    _route_func = rf;

    map<string, tRoutingFunction>::const_iterator rf_iter = gRoutingFunctionMap.find(rf);
    if(rf_iter == gRoutingFunctionMap.end()) {
        Error("Invalid routing function: " + rf);
    }
    _rf = rf_iter->second;

    // Alloc VC's
    _buf.resize(_inputs);
    for ( int i = 0; i < _inputs; ++i ) {
        ostringstream module_name;
        module_name << "buf_" << i;
        _buf[i] = new Buffer(config, _outputs, this, module_name.str( ) );
        module_name.str("");
    }

    // Alloc next VCs' buffer state
    _next_buf.resize(_outputs);
    for (int j = 0; j < _outputs; ++j) {
        ostringstream module_name;
        module_name << "next_vc_o" << j;
        _next_buf[j] = new BufferState( config, this, module_name.str( ) );
        module_name.str("");
    }

    // Alloc allocators
    string vc_alloc_type = config.GetStr( "vc_allocator" );

    if(vc_alloc_type == "piggyback") {
        if(!_speculative) {
            Error("Piggyback VC allocation requires speculative switch allocation to be enabled.");
        }
        _vc_allocator = NULL;
        _vc_rr_offset.resize(_outputs*_classes, -1);
    } else {
        _vc_allocator = Allocator::NewAllocator( this, "vc_allocator",
                vc_alloc_type,
                _vcs*_inputs,
                _vcs*_outputs );

        if ( !_vc_allocator ) {
            Error("Unknown vc_allocator type: " + vc_alloc_type);
        }
    }

    // Mike: according to the user guide, section 4.6, multiple allocators can be used.
    // "select" is one that gives preference to high priority packets.
    string sw_alloc_type = config.GetStr( "sw_allocator" );
    _sw_allocator = Allocator::NewAllocator( this, "sw_allocator",
            sw_alloc_type,
            _inputs*_input_speedup,
            _outputs*_output_speedup );

    if ( !_sw_allocator ) {
        Error("Unknown sw_allocator type: " + sw_alloc_type);
    }

    // Mike: speculative allocator related settings, don't think we are using this
    // at least not at this point.
    string spec_sw_alloc_type = config.GetStr( "spec_sw_allocator" );
    if ( _speculative && ( spec_sw_alloc_type != "prio" ) ) {
        _spec_sw_allocator = Allocator::NewAllocator( this, "spec_sw_allocator",
                spec_sw_alloc_type,
                _inputs*_input_speedup,
                _outputs*_output_speedup );
        if ( !_spec_sw_allocator ) {
            Error("Unknown spec_sw_allocator type: " + spec_sw_alloc_type);
        }
    } else {
        _spec_sw_allocator = NULL;
    }

    //Mike: ignore the input speed up for now
    _sw_rr_offset.resize(_inputs*_input_speedup);
    for(int i = 0; i < _inputs*_input_speedup; ++i)
        _sw_rr_offset[i] = i % _input_speedup;

    // next hop on queue for lookahead routing
    _noq = config.GetInt("noq") > 0;
    if(_noq) {
        if(_routing_delay) {
            Error("NOQ requires lookahead routing to be enabled.");
        }
        if(_vcs < _outputs) {
            Error("NOQ requires at least as many VCs as router outputs.");
        }
    }
    _noq_next_output_port.resize(_inputs, vector<int>(_vcs, -1));
    _noq_next_vc_start.resize(_inputs, vector<int>(_vcs, -1));
    _noq_next_vc_end.resize(_inputs, vector<int>(_vcs, -1));

    // Output queues
    _output_buffer_size = config.GetInt("output_buffer_size");
    _output_buffer.resize(_outputs);
    _credit_buffer.resize(_inputs);

    // Switch configuration (when held for multiple cycles)
    _hold_switch_for_packet = (config.GetInt("hold_switch_for_packet") > 0);
    _switch_hold_in.resize(_inputs*_input_speedup, -1);
    _switch_hold_out.resize(_outputs*_output_speedup, -1);
    _switch_hold_vc.resize(_inputs*_input_speedup, -1);

    // Safety check for express virtual channels
    if (_evc)
    {
        if (_vcs < 2)
        {
            Error("Must have at least 2 virtual channels if express virtual channel is enabled.");
        }
        if (!_hold_switch_for_evc_packet)
        {
            Error("Must enable hold_switch_for_packet if express virtual channel is enabled.");
        }
        if (_evc_prioritized_class >= _classes)
        {
            Error("EVC prioritized class is not a valid class id.");
        }
    }

    _bufferMonitor = new BufferMonitor(inputs, _classes);
    _switchMonitor = new SwitchMonitor(inputs, outputs, _classes);

#ifdef TRACK_FLOWS
    for(int c = 0; c < _classes; ++c) {
        _stored_flits[c].resize(_inputs, 0);
        _active_packets[c].resize(_inputs, 0);
    }
    _outstanding_classes.resize(_outputs, vector<queue<int> >(_vcs));
#endif
}

IQEVCRouter::~IQEVCRouter( )
{

    if(gPrintActivity) {
        cout << Name() << ".bufferMonitor:" << endl ;
        cout << *_bufferMonitor << endl ;

        cout << Name() << ".switchMonitor:" << endl ;
        cout << "Inputs=" << _inputs ;
        cout << "Outputs=" << _outputs ;
        cout << *_switchMonitor << endl ;
    }

    for(int i = 0; i < _inputs; ++i)
        delete _buf[i];

    for(int j = 0; j < _outputs; ++j)
        delete _next_buf[j];

    delete _vc_allocator;
    delete _sw_allocator;
    if(_spec_sw_allocator)
        delete _spec_sw_allocator;

    delete _bufferMonitor;
    delete _switchMonitor;
}

void IQEVCRouter::AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
    int alloc_delay = _speculative ? max(_vc_alloc_delay, _sw_alloc_delay) : (_vc_alloc_delay + _sw_alloc_delay);
    int min_latency = 1 + _crossbar_delay + channel->GetLatency() + _routing_delay + alloc_delay + backchannel->GetLatency()  + _credit_delay;
    _next_buf[_output_channels.size()]->SetMinLatency(min_latency);
    Router::AddOutputChannel(channel, backchannel);
}

void IQEVCRouter::ReadInputs( )
{
    bool have_flits = _ReceiveFlits( );
    bool have_credits = _ReceiveCredits( );
    _active = _active || have_flits || have_credits;
}

//jason: note that different stages of the router pipeline don't necessarily have to be
// performed in order in software because its a pipeline! for example, SWAllocEvaluate
// is performed before RouteUpdate in software. The results are pushed onto vector sets.
void IQEVCRouter::_InternalStep( )
{
    if(!_active) {
        return;
    }


    //mike: Buffer Write
    _InputQueuing( );
    bool activity = !_proc_credits.empty();

    //mike: list of head flits that need to go through routing stage.
    if(!_route_vcs.empty())
        _RouteEvaluate( );

    if(_vc_allocator) {
        _vc_allocator->Clear();
        if(!_vc_alloc_vcs.empty())
            _VCAllocEvaluate( );
    }

    //Mike: hold the switch based on config, or if it's EVC packet
    if((_hold_switch_for_packet) || (_hold_switch_for_evc_packet && _evc)) {
        //mike: EVC needs to be added to this data structure.
        if(!_sw_hold_vcs.empty())
            _SWHoldEvaluate( );
    }
    _sw_allocator->Clear();
    if(_spec_sw_allocator)
        _spec_sw_allocator->Clear();
    if(!_sw_alloc_vcs.empty())
        _SWAllocEvaluate( );
    if(!_crossbar_flits.empty())
        _SwitchEvaluate( );

    //jason: routing function computation
    if(!_route_vcs.empty()) {
        _RouteUpdate( );
        activity = activity || !_route_vcs.empty();
    }
    if(!_vc_alloc_vcs.empty()) {
        _VCAllocUpdate( );
        activity = activity || !_vc_alloc_vcs.empty();
    }
    if((_hold_switch_for_packet) || (_hold_switch_for_evc_packet && _evc)) {
        if(!_sw_hold_vcs.empty()) {
            _SWHoldUpdate( );
            activity = activity || !_sw_hold_vcs.empty();
        }
    }
    if(!_sw_alloc_vcs.empty()) {
        _SWAllocUpdate( );
        activity = activity || !_sw_alloc_vcs.empty();
    }
    if(!_crossbar_flits.empty()) {
        _SwitchUpdate( );
        activity = activity || !_crossbar_flits.empty();
    }

    _active = activity;

    _OutputQueuing( );

    _bufferMonitor->cycle( );
    _switchMonitor->cycle( );
}

void IQEVCRouter::WriteOutputs( )
{
    _SendFlits( );
    _SendCredits( );
}


//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool IQEVCRouter::_ReceiveFlits( )
{
    bool activity = false;
    for(int input = 0; input < _inputs; ++input) {
        Flit * const f = _input_channels[input]->Receive();
        if(f) {

#ifdef TRACK_FLOWS
            ++_received_flits[f->cl][input];
#endif

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "Received flit " << f->id
                    << " from channel at input " << input
                    << "." << endl;
            }
            _in_queue_flits.insert(make_pair(input, f));
            activity = true;
        }
    }
    return activity;
}

bool IQEVCRouter::_ReceiveCredits( )
{
    bool activity = false;
    for(int output = 0; output < _outputs; ++output) {
        Credit * const c = _output_credits[output]->Receive();
        if(c) {
            _proc_credits.push_back(make_pair(GetSimTime() + _credit_delay,
                        make_pair(c, output)));
            activity = true;
        }
    }
    return activity;
}


//------------------------------------------------------------------------------
// input queuing
//------------------------------------------------------------------------------

void IQEVCRouter::_InputQueuing( )
{
    for(map<int, Flit *>::const_iterator iter = _in_queue_flits.begin();
            iter != _in_queue_flits.end();
            ++iter) {

        int const input = iter->first;
        assert((input >= 0) && (input < _inputs));

        Flit * const f = iter->second;
        assert(f);

        int const vc = f->vc;
        assert((vc >= 0) && (vc < _vcs));

        Buffer * const cur_buf = _buf[input];

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Adding flit " << f->id
                << " to VC " << vc
                << " at input " << input
                << " (state: " << VC::VCSTATE[cur_buf->GetState(vc)];
            if(cur_buf->Empty(vc)) {
                *gWatchOut << ", empty";
            } else {
                assert(cur_buf->FrontFlit(vc));
                *gWatchOut << ", front: " << cur_buf->FrontFlit(vc)->id;
            }
            *gWatchOut << ")." << endl;
        }

        //mike: buffer write stage, adds flit to the VC
        cur_buf->AddFlit(vc, f);

#ifdef TRACK_FLOWS
        ++_stored_flits[f->cl][input];
        if(f->head) ++_active_packets[f->cl][input];
#endif

        _bufferMonitor->write(input, f) ;


        //mike: when the tail flit leaves, VC goes into idle state
        if(cur_buf->GetState(vc) == VC::idle) {
            assert(cur_buf->FrontFlit(vc) == f);
            assert(cur_buf->GetOccupancy(vc) == 1);
            assert(f->head);
            assert(_switch_hold_vc[input*_input_speedup + vc%_input_speedup] != vc);
            if(_routing_delay) {
                cur_buf->SetState(vc, VC::routing);
                _route_vcs.push_back(make_pair(-1, make_pair(input, vc)));
            } else {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "Using precomputed lookahead routing information for VC " << vc
                        << " at input " << input
                        << " (front: " << f->id
                        << ")." << endl;
                }
                cur_buf->SetRouteSet(vc, &f->la_route_set);
                cur_buf->SetState(vc, VC::vc_alloc);
                if(_speculative) {
                    _sw_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                    -1)));
                }
                if(_vc_allocator) {
                    _vc_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                    -1)));
                }
                if(_noq) {
                    _UpdateNOQ(input, vc, f);
                }
            }
        } else if((cur_buf->GetState(vc) == VC::active) &&
                (cur_buf->FrontFlit(vc) == f)) {
            if(_switch_hold_vc[input*_input_speedup + vc%_input_speedup] == vc) {
                _sw_hold_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                -1)));
            } else {
                _sw_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                -1)));
            }
        }
    }

    //mike: done processing queue, clears them
    _in_queue_flits.clear();

    //mike: adjust/pop the credit
    while(!_proc_credits.empty()) {

        pair<int, pair<Credit *, int> > const & item = _proc_credits.front();

        int const time = item.first;
        if(GetSimTime() < time) {
            break;
        }

        Credit * const c = item.second.first;
        assert(c);

        int const output = item.second.second;
        assert((output >= 0) && (output < _outputs));

        BufferState * const dest_buf = _next_buf[output];

#ifdef TRACK_FLOWS
        for(set<int>::const_iterator iter = c->vc.begin(); iter != c->vc.end(); ++iter) {
            int const vc = *iter;
            assert(!_outstanding_classes[output][vc].empty());
            int cl = _outstanding_classes[output][vc].front();
            _outstanding_classes[output][vc].pop();
            assert(_outstanding_credits[cl][output] > 0);
            --_outstanding_credits[cl][output];
        }
#endif

        dest_buf->ProcessCredit(c);
        c->Free();
        _proc_credits.pop_front();
    }
}


//------------------------------------------------------------------------------
// routing
//------------------------------------------------------------------------------

void IQEVCRouter::_RouteEvaluate( )
{
    assert(_routing_delay);

    for(deque<pair<int, pair<int, int> > >::iterator iter = _route_vcs.begin();
            iter != _route_vcs.end();
            ++iter) {

        int const time = iter->first;
        if(time >= 0) {
            break;
        }
        iter->first = GetSimTime() + _routing_delay - 1;

        int const input = iter->second.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.second;
        assert((vc >= 0) && (vc < _vcs));

        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::routing);

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);
        assert(f->head);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Beginning routing for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }
    }
}

void IQEVCRouter::_RouteUpdate( )
{
    assert(_routing_delay);

    while(!_route_vcs.empty()) {

        pair<int, pair<int, int> > const & item = _route_vcs.front();

        int const time = item.first;
        if((time < 0) || (GetSimTime() < time)) {
            break;
        }
        assert(GetSimTime() == time);

        int const input = item.second.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = item.second.second;
        assert((vc >= 0) && (vc < _vcs));

        Buffer * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::routing);

        Flit * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);
        assert(f->head);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Completed routing for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        // jason: Route function
        cur_buf->Route(vc, _rf, this, f, input);
        cur_buf->SetState(vc, VC::vc_alloc);

        if (_evc_next_route) {
            // jason: Compute future output port at next router
            OutputSet const * const route_set = cur_buf->GetRouteSet(vc);
            set<OutputSet::sSetElement> const setlist = route_set->GetSet();
            for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin(); iset != setlist.end(); ++iset) {
                // get the output port
                int const out_port = iset->output_port;

                // get flit channel corresponding to that output port; this will be used to find the downstream router id
                FlitChannel *fc_out = _output_channels[out_port];
                cur_buf->NextRoute(vc, _route_func, this, fc_out, f);
            }
        }

        if (_speculative) {
            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second, -1)));
        }
        if(_vc_allocator) {
            _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second, -1)));
        }
        // NOTE: No need to handle NOQ here, as it requires lookahead routing!
        _route_vcs.pop_front();
    }
}


//------------------------------------------------------------------------------
// VC allocation
//------------------------------------------------------------------------------

void IQEVCRouter::_VCAllocEvaluate( )
{
    assert(_vc_allocator);

    bool watched = false;

    //mike: _vc_alloc_vcs: only contains head flits, done in the input queuing part (only push head flits in there)
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
            iter != _vc_alloc_vcs.end();
            ++iter) {

        //mike: time -> actual simulation time, if it's has been processed, skip it.
        int const time = iter->first;
        if(time >= 0) {
            break;
        }

        int const input = iter->second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        assert(iter->second.second == -1);

        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::vc_alloc);

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);
        assert(f->head);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Beginning VC allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        //mike: retrieving the actual routing information, VC start, VC end, priority (priority used the routing function, output port
        OutputSet const * const route_set = cur_buf->GetRouteSet(vc);
        assert(route_set);

        int const out_priority = cur_buf->GetPriority(vc);
        set<OutputSet::sSetElement> const setlist = route_set->GetSet();

        //mike: elig: eligible, if this head flit has been assigned an output port.
        bool elig = false;
        bool cred = false;
        bool reserved = false;

        assert(!_noq || (setlist.size() == 1));

        //mike: go through list of output ports assigned from the routing function, only 1
        //mike: retrieves the buffer state, how it knows about the downstream router
        for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin(); iset != setlist.end(); ++iset) {

            int const out_port = iset->output_port;
            assert((out_port >= 0) && (out_port < _outputs));

            //mike:destination buffer, downstream buffer state
            BufferState *dest_buf = _next_buf[out_port];

            //mike: absolute VC for a given router, called TAG, 0 to total number of VCs under inputs
            int vc_start;
            int vc_end;

            if(_noq && _noq_next_output_port[input][vc] >= 0) {
                assert(!_routing_delay);
                //jason: check if head flit is of class 1 or class 0 (same as below)
                if (f->cl == _evc_prioritized_class) {
                    vc_end = _vcs - 1; //iset->vc_end;
                    vc_start = _vcs - _num_evcs;
                } else {
                    vc_start = 0; //iset->vc_start;
                    vc_end = _vcs - 1 - _num_evcs; //iset->vc_end - _num_evcs;
                }
            }
            else {
                //jason: check if the head flit is of class 1 or class 0
                // relative vc numbers
                // class 1 gets to use the EVCs (the highest relative VC numbers are destinated as the EVCs)
                // class 0 gets to use the rest of the VCs
                if (f->cl == _evc_prioritized_class) {
                    vc_end = _vcs - 1;
                    vc_start = _vcs - _num_evcs;
                } else {
                    vc_start = 0;
                    vc_end = _vcs - 1 - _num_evcs;
                }
            }

            //jason: print out VC start and end
            //printf("Info: flit class:%u, vc_start:%u, vc_end:%u\n", f->cl, vc_start, vc_end);

            assert(vc_start >= 0 && vc_start < _vcs);
            assert(vc_end >= 0 && vc_end < _vcs);
            assert(vc_end >= vc_start);

            int cur_next_out_port;
            if (_evc_next_route) {
                //jason: next output port set for the current head flit
                cur_next_out_port = cur_buf->GetNextOutputPort(vc);

                // clear (set to -1) dest buffer next_output_ports if the downstream buffers are empty
                for (int i_vc = 0; i_vc < _vcs; ++i_vc) {
                    if (dest_buf->OccupancyFor(i_vc) == 0) {
                        dest_buf->SetNextOutputPort(i_vc, -1);
                    }
                }
            }

            // start requesting
            for(int out_vc = vc_start; out_vc <= vc_end; ++out_vc) {
                assert((out_vc >= 0) && (out_vc < _vcs));

                int in_priority = iset->pri;
                assert(in_priority >= 0);

                //jason: selective VC request based on VC occupancy
                if (_selective_vc_request)
                    in_priority += 50 - dest_buf->OccupancyFor(out_vc);

                int dest_next_out_port;
                if (_evc_next_route) {
                    //jason: next output port of the last flit that was sent to the corresponding downstream VC
                    dest_next_out_port = dest_buf->GetNextOutputPort(out_vc);

                    // if there is an empty buffer available, then push that one into the list of vc_set (in outputset) and set to a very high priority
                    if (dest_next_out_port == -1) {
                        in_priority += 50; // jason: arbitrarily picked 50 because its a larger number relative to the number of VCs you would normally have
                    }

                    // use the future output set that was calculated from the NextRoute in routing stage and see if there exists a downstream vc that is assigned to the same outputport; level up the priority if output ports match
                    if (dest_next_out_port == cur_next_out_port) {
                        in_priority += 30;
                    }
                }

                //jason: don't enable prioritize empty for now, since we don't want this assignment
                if(_vc_prioritize_empty && !dest_buf->IsEmptyFor(out_vc)) 
                    in_priority += numeric_limits<int>::min();
                //printf("out_vc:%u, in_pri:%u\n", out_vc, in_priority);

                // On the input input side, a VC might request several output VCs (downstream VCs).
                // These VCs can be prioritized by the routing function, and this is
                // reflected in "in_priority". On the output side, if multiple VCs are
                // requesting the same output VC, the priority of VCs is based on the
                // actual packet priorities, which is reflected in "out_priority".

                //jason: if destination buffer is not available, don't request for the vc
                if(!dest_buf->IsAvailableFor(out_vc)) {
                    if(f->watch) {
                        int const use_input_and_vc = dest_buf->UsedBy(out_vc);
                        int const use_input = use_input_and_vc / _vcs;
                        int const use_vc = use_input_and_vc % _vcs;
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                            << "  VC " << out_vc
                            << " at output " << out_port
                            << " is in use by VC " << use_vc
                            << " at input " << use_input;
                        Flit * cf = _buf[use_input]->FrontFlit(use_vc);
                        if(cf) {
                            *gWatchOut << " (front flit: " << cf->id << ")";
                        } else {
                            *gWatchOut << " (empty)";
                        }
                        *gWatchOut << "." << endl;
                    }
                } else {
                    //mike: destination buffer is available, set elig=true
                    elig = true;
                    if(_vc_busy_when_full && dest_buf->IsFullFor(out_vc)) {
                        if(f->watch)
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "  VC " << out_vc
                                << " at output " << out_port
                                << " is full." << endl;
                        reserved |= !dest_buf->IsFull();
                    } else {
                        //mike: credit exists
                        cred = true;
                        if(f->watch){
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "  Requesting VC " << out_vc
                                << " at output " << out_port
                                << " (in_pri: " << in_priority
                                << ", out_pri: " << out_priority
                                << ")." << endl;
                            watched = true;
                        }
                        //mike: where it links input VC with output port
                        //jason: temporary print out the requested VC numbers
                        //printf("flit class: %u, input vc: %u, output vc: %u\n", f->cl, vc, out_vc);
                        _vc_allocator->AddRequest(input*_vcs + vc, out_port*_vcs + out_vc,
                                0, in_priority, out_priority);
                    }
                }
            }
        }
        if(!elig) {
            iter->second.second = STALL_BUFFER_BUSY;
        } else if(_vc_busy_when_full && !cred) {
            iter->second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
        }
    }

    if(watched) {
        *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | ";
        _vc_allocator->PrintRequests( gWatchOut );
    }

    //mike: where outputs are assigned to out match variable, grants the request.
    //mike: if one is denied, could request a different one.
    _vc_allocator->Allocate();

    if(watched) {
        *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | ";
        _vc_allocator->PrintGrants( gWatchOut );
    }

    //mike: calculate simulation times, only if outputs are assigned.
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
            iter != _vc_alloc_vcs.end();
            ++iter) {

        int const time = iter->first;
        if(time >= 0) {
            break;
        }
        iter->first = GetSimTime() + _vc_alloc_delay - 1;

        int const input = iter->second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        if(iter->second.second < -1) {
            continue;
        }

        assert(iter->second.second == -1);

        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::vc_alloc);

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);
        assert(f->head);

        //mike: did this item in the queue get its request granted
        int const output_and_vc = _vc_allocator->OutputAssigned(input * _vcs + vc);

        //mike: if it is granted
        if(output_and_vc >= 0) {
            // get the assigned output port
            int cur_next_out_port = cur_buf->GetNextOutputPort(vc);

            int const match_output = output_and_vc / _vcs;
            assert((match_output >= 0) && (match_output < _outputs));
            int const match_vc = output_and_vc % _vcs;
            assert((match_vc >= 0) && (match_vc < _vcs));

            if (_evc_next_route) {
                //jason: set the next output port buffer state for the downstream VC that was assigned
                OutputSet const * const route_set = cur_buf->GetRouteSet(vc);
                assert(route_set);
                set<OutputSet::sSetElement> const setlist = route_set->GetSet();
                for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin(); iset != setlist.end(); ++iset) {
                    int const out_port = iset->output_port;
                    assert((out_port >= 0) && (out_port < _outputs));
                    BufferState *dest_buf = _next_buf[out_port];

                    // set next output port for downstream buffer state
                    dest_buf->SetNextOutputPort(match_vc, cur_next_out_port);
                }
            }

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "Assigning VC " << match_vc
                    << " at output " << match_output
                    << " to VC " << vc
                    << " at input " << input
                    << "." << endl;
            }
            //mike: indicator, output/VC actually assigned.
            //mike: output_and_VC: VC # of the downstream router.
            iter->second.second = output_and_vc;

        } else {

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "VC allocation failed for VC " << vc
                    << " at input " << input
                    << "." << endl;
            }

            iter->second.second = STALL_BUFFER_CONFLICT;

        }
    }

    if(_vc_alloc_delay <= 1) {
        return;
    }

    //mike: checking whether VC is still available
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
            iter != _vc_alloc_vcs.end();
            ++iter) {

        int const time = iter->first;
        assert(time >= 0);
        if(GetSimTime() < time) {
            break;
        }

        assert(iter->second.second != -1);

        int const output_and_vc = iter->second.second;

        if(output_and_vc >= 0) {

            int const match_output = output_and_vc / _vcs;
            assert((match_output >= 0) && (match_output < _outputs));
            int const match_vc = output_and_vc % _vcs;
            assert((match_vc >= 0) && (match_vc < _vcs));

            BufferState const * const dest_buf = _next_buf[match_output];

            int const input = iter->second.first.first;
            assert((input >= 0) && (input < _inputs));
            int const vc = iter->second.first.second;
            assert((vc >= 0) && (vc < _vcs));

            Buffer const * const cur_buf = _buf[input];
            assert(!cur_buf->Empty(vc));
            assert(cur_buf->GetState(vc) == VC::vc_alloc);

            Flit const * const f = cur_buf->FrontFlit(vc);
            assert(f);
            assert(f->vc == vc);
            assert(f->head);

            if(!dest_buf->IsAvailableFor(match_vc)) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Discarding previously generated grant for VC " << vc
                        << " at input " << input
                        << ": VC " << match_vc
                        << " at output " << match_output
                        << " is no longer available." << endl;
                }
                iter->second.second = STALL_BUFFER_BUSY;
            } else if(_vc_busy_when_full && dest_buf->IsFullFor(match_vc)) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Discarding previously generated grant for VC " << vc
                        << " at input " << input
                        << ": VC " << match_vc
                        << " at output " << match_output
                        << " has become full." << endl;
                }
                iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
            }
        }
    }
}

void IQEVCRouter::_VCAllocUpdate( )
{
    assert(_vc_allocator);

    //mike: goes through the queue, done when it's empty.

    while(!_vc_alloc_vcs.empty()) {

        pair<int, pair<pair<int, int>, int> > const & item = _vc_alloc_vcs.front();

        int const time = item.first;
        if((time < 0) || (GetSimTime() < time)) {
            break;
        }
        assert(GetSimTime() == time);

        int const input = item.second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = item.second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        assert(item.second.second != -1);

        Buffer * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::vc_alloc);

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);
        assert(f->head);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Completed VC allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        //jason: output_and_vc contains the absolute VC number of the downstream VC the packet is assigned to
        int const output_and_vc = item.second.second;

        if(output_and_vc >= 0) {

            int const match_output = output_and_vc / _vcs;
            assert((match_output >= 0) && (match_output < _outputs));
            int const match_vc = output_and_vc % _vcs;
            assert((match_vc >= 0) && (match_vc < _vcs));

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Acquiring assigned VC " << match_vc
                    << " at output " << match_output
                    << "." << endl;
            }

            BufferState * const dest_buf = _next_buf[match_output];
            assert(dest_buf->IsAvailableFor(match_vc));

            //mike: reserves VC buffer at the destination buffer.
            //mike: put into next buffer, setup output, sets VC active.
            dest_buf->TakeBuffer(match_vc, input*_vcs + vc);

            cur_buf->SetOutput(vc, match_output, match_vc);
            cur_buf->SetState(vc, VC::active);




            if(!_speculative) {
                _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
            }
        } else {
            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  No output VC allocated." << endl;
            }

#ifdef TRACK_STALLS
            assert((output_and_vc == STALL_BUFFER_BUSY) ||
                    (output_and_vc == STALL_BUFFER_CONFLICT));
            if(output_and_vc == STALL_BUFFER_BUSY) {
                ++_buffer_busy_stalls[f->cl];
            } else if(output_and_vc == STALL_BUFFER_CONFLICT) {
                ++_buffer_conflict_stalls[f->cl];
            }
#endif

            _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
        }
        _vc_alloc_vcs.pop_front();
    }
}


//------------------------------------------------------------------------------
// switch holding
//------------------------------------------------------------------------------

void IQEVCRouter::_SWHoldEvaluate( )
{
    // Mike: make sure you should be here
    assert(_hold_switch_for_packet || (_hold_switch_for_evc_packet && _evc));

    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_hold_vcs.begin();
            iter != _sw_hold_vcs.end();
            ++iter) {

        int const time = iter->first;
        if(time >= 0) {
            break;
        }
        iter->first = GetSimTime();

        int const input = iter->second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        assert(iter->second.second == -1);

        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::active);

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Beginning held switch allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        int const expanded_input = input * _input_speedup + vc % _input_speedup;
        assert(_switch_hold_vc[expanded_input] == vc);

        int const match_port = cur_buf->GetOutputPort(vc);
        assert((match_port >= 0) && (match_port < _outputs));
        int const match_vc = cur_buf->GetOutputVC(vc);
        assert((match_vc >= 0) && (match_vc < _vcs));

        int const expanded_output = match_port*_output_speedup + input%_output_speedup;
        assert(_switch_hold_in[expanded_input] == expanded_output);

        BufferState const * const dest_buf = _next_buf[match_port];

        // Check if the destination is full or not
        if(dest_buf->IsFullFor(match_vc)) {
            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Unable to reuse held connection from input " << input
                    << "." << (expanded_input % _input_speedup)
                    << " to output " << match_port
                    << "." << (expanded_output % _output_speedup)
                    << ": No credit available." << endl;
            }
            iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
        } else {
            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Reusing held connection from input " << input
                    << "." << (expanded_input % _input_speedup)
                    << " to output " << match_port
                    << "." << (expanded_output % _output_speedup)
                    << "." << endl;
            }
            iter->second.second = expanded_output;
        }
    }
}

void IQEVCRouter::_SWHoldUpdate( )
{
    assert(_hold_switch_for_packet || (_hold_switch_for_evc_packet && _evc));

    while(!_sw_hold_vcs.empty()) {

        pair<int, pair<pair<int, int>, int> > const & item = _sw_hold_vcs.front();

        int const time = item.first;
        if(time < 0) {
            break;
        }
        assert(GetSimTime() == time);

        int const input = item.second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = item.second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        assert(item.second.second != -1);

        Buffer * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert(cur_buf->GetState(vc) == VC::active);

        Flit * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Completed held switch allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        int const expanded_input = input * _input_speedup + vc % _input_speedup;
        assert(_switch_hold_vc[expanded_input] == vc);

        int const expanded_output = item.second.second;

        if(expanded_output >= 0 && ( _output_buffer_size==-1 || _output_buffer[expanded_output].size()<size_t(_output_buffer_size))) {

            assert(_switch_hold_in[expanded_input] == expanded_output);
            assert(_switch_hold_out[expanded_output] == expanded_input);

            int const output = expanded_output / _output_speedup;
            assert((output >= 0) && (output < _outputs));
            assert(cur_buf->GetOutputPort(vc) == output);

            int const match_vc = cur_buf->GetOutputVC(vc);
            assert((match_vc >= 0) && (match_vc < _vcs));

            BufferState * const dest_buf = _next_buf[output];

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Scheduling switch connection from input " << input
                    << "." << (vc % _input_speedup)
                    << " to output " << output
                    << "." << (expanded_output % _output_speedup)
                    << "." << endl;
            }

            cur_buf->RemoveFlit(vc);

#ifdef TRACK_FLOWS
            --_stored_flits[f->cl][input];
            if(f->tail) --_active_packets[f->cl][input];
#endif

            _bufferMonitor->read(input, f) ;

            f->hops++;
            f->vc = match_vc;

            if(!_routing_delay && f->head) {
                const FlitChannel * channel = _output_channels[output];
                const Router * router = channel->GetSink();
                if(router) {
                    if(_noq) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Updating lookahead routing information for flit " << f->id
                                << " (NOQ)." << endl;
                        }
                        int next_output_port = _noq_next_output_port[input][vc];
                        assert(next_output_port >= 0);
                        _noq_next_output_port[input][vc] = -1;
                        int next_vc_start = _noq_next_vc_start[input][vc];
                        assert(next_vc_start >= 0 && next_vc_start < _vcs);
                        _noq_next_vc_start[input][vc] = -1;
                        int next_vc_end = _noq_next_vc_end[input][vc];
                        assert(next_vc_end >= 0 && next_vc_end < _vcs);
                        _noq_next_vc_end[input][vc] = -1;
                        f->la_route_set.Clear();
                        f->la_route_set.AddRange(next_output_port, next_vc_start, next_vc_end);
                    } else {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Updating lookahead routing information for flit " << f->id
                                << "." << endl;
                        }
                        int in_channel = channel->GetSinkPort();
                        _rf(router, f, in_channel, &f->la_route_set, false);
                    }
                } else {
                    f->la_route_set.Clear();
                }
            }

#ifdef TRACK_FLOWS
            ++_outstanding_credits[f->cl][output];
            _outstanding_classes[output][f->vc].push(f->cl);
#endif
            dest_buf->SendingFlit(f);

            _crossbar_flits.push_back(make_pair(-1, make_pair(f, make_pair(expanded_input, expanded_output))));

            if(_out_queue_credits.count(input) == 0) {
                _out_queue_credits.insert(make_pair(input, Credit::New()));
            }
            _out_queue_credits.find(input)->second->vc.insert(vc);

            //mike: loop through EVCs to check if any held connections need to yield
            int waiting_vc = -1;
            int waiting_output = -1;
            int EVC_coming = 0;
            for(waiting_vc = _vcs - _num_evcs; waiting_vc < _vcs-1; waiting_vc++){
                if (!cur_buf->Empty(waiting_vc) && f->cl != _evc_prioritized_class && cur_buf->FrontFlit(waiting_vc)->head && cur_buf->GetOutputPort(waiting_vc) != -1 && cur_buf->FrontFlit(waiting_vc)->cl==_evc_prioritized_class) {
                    //printf("holding in: %d, VC: %d, out: %d, flit: %d, pri: %d, coming in: %d, VC: %d, out: %d, flit: %d, pri: %d\n",expanded_input,vc,expanded_output,f->id, f->pri,expanded_input,cur_buf->FrontFlit(waiting_vc)->vc,cur_buf->GetOutputPort(waiting_vc),cur_buf->FrontFlit(waiting_vc)->id,cur_buf->FrontFlit(waiting_vc)->pri );
                    waiting_output = cur_buf->GetOutputPort(waiting_vc);
                    EVC_coming = 1;
                    break;
                }

            }

            //printf("holding for in: %d, VC: %d, out: %d, flit: %d, pri: %d, EVC waiting? %d\n",expanded_input,vc,expanded_output,f->id, f->pri,EVC_coming );

            if(cur_buf->Empty(vc)) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Cancelling held connection from input " << input
                        << "." << (expanded_input % _input_speedup)
                        << " to " << output
                        << "." << (expanded_output % _output_speedup)
                        << ": No more flits." << endl;
                }
                _switch_hold_vc[expanded_input] = -1;
                _switch_hold_in[expanded_input] = -1;
                _switch_hold_out[expanded_output] = -1;
                if(f->tail) {
                    cur_buf->SetState(vc, VC::idle);
                }
            } else if(_hold_switch_for_evc_packet && _hold_switch_for_packet && EVC_coming && !f->tail){
                //mike: this is so normal VCs yield to EVCs
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Cancelling held connection from input " << input
                        << "." << (expanded_input % _input_speedup)
                        << " to " << output
                        << "." << (expanded_output % _output_speedup)
                        << ": Yield to high priority flit." << endl;
                }
                //printf("changing from: %d/%d->%d to %d/%d->%d\n", _switch_hold_in[expanded_input], _switch_hold_vc[expanded_input], _switch_hold_out[expanded_output], expanded_input, waiting_vc, waiting_output );
                _switch_hold_vc[expanded_input] = -1;
                _switch_hold_in[expanded_input] = -1;
                _switch_hold_out[expanded_output] = -1;
                _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                -1)));
                //_switch_hold_vc[expanded_input] = waiting_vc;
                //_switch_hold_in[expanded_input] = waiting_output;
                //_sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,	-1)));
            } else {
                Flit * const nf = cur_buf->FrontFlit(vc);
                assert(nf);
                assert(nf->vc == vc);
                if(f->tail) {
                    assert(nf->head);
                    if(f->watch) {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                            << "  Cancelling held connection from input " << input
                            << "." << (expanded_input % _input_speedup)
                            << " to " << output
                            << "." << (expanded_output % _output_speedup)
                            << ": End of packet." << endl;
                    }
                    _switch_hold_vc[expanded_input] = -1;
                    _switch_hold_in[expanded_input] = -1;
                    _switch_hold_out[expanded_output] = -1;
                    if(_routing_delay) {
                        cur_buf->SetState(vc, VC::routing);
                        _route_vcs.push_back(make_pair(-1, item.second.first));
                    } else {
                        if(nf->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Using precomputed lookahead routing information for VC " << vc
                                << " at input " << input
                                << " (front: " << nf->id
                                << ")." << endl;
                        }
                        cur_buf->SetRouteSet(vc, &nf->la_route_set);
                        cur_buf->SetState(vc, VC::vc_alloc);
                        if(_speculative) {
                            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                            -1)));
                        }
                        if(_vc_allocator) {
                            _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                            -1)));
                        }
                        if(_noq) {
                            _UpdateNOQ(input, vc, nf);
                        }
                    }
                } else {
                    _sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                    -1)));
                }
            }
        } else {
            //when internal speedup >1.0, the buffer stall stats may not be accruate
            assert((expanded_output == STALL_BUFFER_FULL) ||
                    (expanded_output == STALL_BUFFER_RESERVED) || !( _output_buffer_size==-1 || _output_buffer[expanded_output].size()<size_t(_output_buffer_size)));

            int const held_expanded_output = _switch_hold_in[expanded_input];
            assert(held_expanded_output >= 0);

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Cancelling held connection from input " << input
                    << "." << (expanded_input % _input_speedup)
                    << " to " << (held_expanded_output / _output_speedup)
                    << "." << (held_expanded_output % _output_speedup)
                    << ": Flit not sent." << endl;
            }
            _switch_hold_vc[expanded_input] = -1;
            _switch_hold_in[expanded_input] = -1;
            _switch_hold_out[held_expanded_output] = -1;
            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                            -1)));
        }
        _sw_hold_vcs.pop_front();
    }

    /*
       int temp_input;
       int temp_vc;

       for (temp_input = 0; temp_input < _inputs; temp_input++) {
       Buffer * const temp_buf = _buf[temp_input];

       for(temp_vc = _vcs - _num_evcs; temp_vc < _vcs-1; temp_vc++){
       if (!temp_buf->Empty(temp_vc)){
       Flit * const temp_flit = temp_buf->FrontFlit(temp_vc);
       if (temp_flit->head && temp_buf->GetOutputPort(temp_vc) != -1 && temp_flit->cl==_evc_prioritized_class) {
       _switch_hold_vc[temp_input] = -1;
       _switch_hold_in[temp_input] = -1;
       _switch_hold_out[temp_buf->GetOutputPort(temp_vc)] = -1;
       }

       }
       }
       }

     */
}


//------------------------------------------------------------------------------
// switch allocation
//------------------------------------------------------------------------------

bool IQEVCRouter::_SWAllocAddReq(int input, int vc, int output)
{
    assert(input >= 0 && input < _inputs);
    assert(vc >= 0 && vc < _vcs);
    assert(output >= 0 && output < _outputs);

    // When input_speedup > 1, the virtual channel buffers are interleaved to
    // create multiple input ports to the switch. Similarily, the output ports
    // are interleaved based on their originating input when output_speedup > 1.

    int const expanded_input = input * _input_speedup + vc % _input_speedup;
    int const expanded_output = output * _output_speedup + input % _output_speedup;

    Buffer const * const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert((cur_buf->GetState(vc) == VC::active) ||
            (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

    Flit const * const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    if((_switch_hold_in[expanded_input] < 0) &&
            (_switch_hold_out[expanded_output] < 0)) {

        Allocator * allocator = _sw_allocator;
        int prio = cur_buf->GetPriority(vc);

        if(_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)) {
            if(_spec_sw_allocator) {
                allocator = _spec_sw_allocator;
            } else {
                assert(prio >= 0);
                prio += numeric_limits<int>::min();
            }
        }

        Allocator::sRequest req;

        if(allocator->ReadRequest(req, expanded_input, expanded_output)) {
            if(RoundRobinArbiter::Supersedes(vc, prio, req.label, req.in_pri,
                        _sw_rr_offset[expanded_input], _vcs)) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Replacing earlier request from VC " << req.label
                        << " for output " << output
                        << "." << (expanded_output % _output_speedup)
                        << " with priority " << req.in_pri
                        << " (" << ((cur_buf->GetState(vc) == VC::active) ?
                                    "non-spec" :
                                    "spec")
                        << ", pri: " << prio
                        << ")." << endl;
                }
                allocator->RemoveRequest(expanded_input, expanded_output, req.label);
                allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
                return true;
            }
            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Output " << output
                    << "." << (expanded_output % _output_speedup)
                    << " was already requested by VC " << req.label
                    << " with priority " << req.in_pri
                    << " (pri: " << prio
                    << ")." << endl;
            }
            return false;
        }
        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "  Requesting output " << output
                << "." << (expanded_output % _output_speedup)
                << " (" << ((cur_buf->GetState(vc) == VC::active) ?
                            "non-spec" :
                            "spec")
                << ", pri: " << prio
                << ")." << endl;
        }
        allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
        return true;
    }
    if(f->watch) {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
            << "  Ignoring output " << output
            << "." << (expanded_output % _output_speedup)
            << " due to switch hold (";
        if(_switch_hold_in[expanded_input] >= 0) {
            *gWatchOut << "input: " << input
                << "." << (expanded_input % _input_speedup);
            if(_switch_hold_out[expanded_output] >= 0) {
                *gWatchOut << ", ";
            }
        }
        if(_switch_hold_out[expanded_output] >= 0) {
            *gWatchOut << "output: " << output
                << "." << (expanded_output % _output_speedup);
        }
        *gWatchOut << ")." << endl;
    }
    return false;
}

void IQEVCRouter::_SWAllocEvaluate( )
{
    bool watched = false;

    // Mike: another one of these structures
    // iter: iterator starting from the beginning of the double ended queue
    // iter->first: time in int
    // iter->second.first.first: input port in int
    // iter->second.first.second: vc # in int
    // iter->second.second: stalling flag
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
            iter != _sw_alloc_vcs.end();
            ++iter) {

        int const time = iter->first;

        if(time >= 0) {
            break;
        }

        int const input = iter->second.first.first;

        //mike: make sure the input is valid
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.first.second;

        //mike: make sure the vc allocated is valid
        assert((vc >= 0) && (vc < _vcs));

        //mike: stalling due to buffer condition
        assert(iter->second.second == -1);

        //mike: make sure the switch is not being held for something else
        //mike: might need to update this for evc
        assert(_switch_hold_vc[input * _input_speedup + vc % _input_speedup] != vc);

        //mike: grab the buffer associated with the input, make sure it's not empty and it's active
        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert((cur_buf->GetState(vc) == VC::active) ||
                (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

        //mike: get the flit at the front of the buffer
        //make sure it's at where it's suppose to be and do VC
        //allocation for this specific VC
        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Beginning switch allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        //mike: check if this VC is active
        if(cur_buf->GetState(vc) == VC::active) {

            //mike: figure out the output associated with the VC
            int const dest_output = cur_buf->GetOutputPort(vc);

            //mike: make sure the destination buffer is a valid one
            assert((dest_output >= 0) && (dest_output < _outputs));

            //mike: dest_vc is the downstream vc
            int const dest_vc = cur_buf->GetOutputVC(vc);
            assert((dest_vc >= 0) && (dest_vc < _vcs));

            //mike: check the destination buffer
            BufferState const * const dest_buf = _next_buf[dest_output];

            if(dest_buf->IsFullFor(dest_vc) || ( _output_buffer_size!=-1  && _output_buffer[dest_output].size()>=(size_t)(_output_buffer_size))) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  VC " << dest_vc
                        << " at output " << dest_output
                        << " is full." << endl;
                }
                iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
                continue;
            }
            bool const requested = _SWAllocAddReq(input, vc, dest_output);
            watched |= requested && f->watch;
            continue;
        }
        assert(_speculative && (cur_buf->GetState(vc) == VC::vc_alloc));

        //mike: make sure it's the head flit.
        assert(f->head);


        //mike: skip this for now, look at lookahead stuff later
        // The following models the speculative VC allocation aspects of the
        // pipeline. An input VC with a request in for an egress virtual channel
        // will also speculatively bid for the switch regardless of whether the VC
        // allocation succeeds.

        OutputSet const * const route_set = cur_buf->GetRouteSet(vc);
        assert(route_set);

        set<OutputSet::sSetElement> const setlist = route_set->GetSet();

        assert(!_noq || (setlist.size() == 1));

        for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
                iset != setlist.end();
                ++iset) {

            int const dest_output = iset->output_port;
            assert((dest_output >= 0) && (dest_output < _outputs));

            // for lower levels of speculation, ignore credit availability and always
            // issue requests for all output ports in route set

            BufferState const * const dest_buf = _next_buf[dest_output];

            bool elig = false;
            bool cred = false;

            if(_spec_check_elig) {

                // for higher levels of speculation, check if at least one suitable VC
                // is available at the current output

                int vc_start;
                int vc_end;

                if(_noq && _noq_next_output_port[input][vc] >= 0) {
                    assert(!_routing_delay);
                    vc_start = _noq_next_vc_start[input][vc];
                    vc_end = _noq_next_vc_end[input][vc];
                } else {
                    vc_start = iset->vc_start;
                    vc_end = iset->vc_end;
                }
                assert(vc_start >= 0 && vc_start < _vcs);
                assert(vc_end >= 0 && vc_end < _vcs);
                assert(vc_end >= vc_start);

                for(int dest_vc = vc_start; dest_vc <= vc_end; ++dest_vc) {
                    assert((dest_vc >= 0) && (dest_vc < _vcs));

                    if(dest_buf->IsAvailableFor(dest_vc) && ( _output_buffer_size==-1 || _output_buffer[dest_output].size()<(size_t)(_output_buffer_size))) {
                        elig = true;
                        if(!_spec_check_cred || !dest_buf->IsFullFor(dest_vc)) {
                            cred = true;
                            break;
                        }
                    }
                }
            }

            if(_spec_check_elig && !elig) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Output " << dest_output
                        << " has no suitable VCs available." << endl;
                }
                iter->second.second = STALL_BUFFER_BUSY;
            } else if(_spec_check_cred && !cred) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  All suitable VCs at output " << dest_output
                        << " are full." << endl;
                }
                iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
            } else {
                bool const requested = _SWAllocAddReq(input, vc, dest_output);
                watched |= requested && f->watch;
            }
        }
    }

    if(watched) {
        *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | ";
        _sw_allocator->PrintRequests(gWatchOut);
        if(_spec_sw_allocator) {
            *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | ";
            _spec_sw_allocator->PrintRequests(gWatchOut);
        }
    }

    //mike: this is the allocator thing, the same as VC allocation part
    //mike: watch out for different types of allocators (refer to user guide)
    //mike: check priorities, which of these items in the queue were granted outputs
    _sw_allocator->Allocate();
    if(_spec_sw_allocator)
        _spec_sw_allocator->Allocate();

    if(watched) {
        *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | ";
        _sw_allocator->PrintGrants(gWatchOut);
        if(_spec_sw_allocator) {
            *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | ";
            _spec_sw_allocator->PrintGrants(gWatchOut);
        }
    }

    //mike: this is where output assignment happens, looks very similar to the previous loop
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
            iter != _sw_alloc_vcs.end();
            ++iter) {

        int const time = iter->first;
        if(time >= 0) {
            break;
        }
        iter->first = GetSimTime() + _sw_alloc_delay - 1;

        int const input = iter->second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = iter->second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        if(iter->second.second < -1) {
            continue;
        }

        assert(iter->second.second == -1);

        Buffer const * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert((cur_buf->GetState(vc) == VC::active) ||
                (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

        Flit const * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);

        int const expanded_input = input * _input_speedup + vc % _input_speedup;

        int expanded_output = _sw_allocator->OutputAssigned(expanded_input);

        if(expanded_output >= 0) {
            assert((expanded_output % _output_speedup) == (input % _output_speedup));
            int const granted_vc = _sw_allocator->ReadRequest(expanded_input, expanded_output);
            if(granted_vc == vc) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "Assigning output " << (expanded_output / _output_speedup)
                        << "." << (expanded_output % _output_speedup)
                        << " to VC " << vc
                        << " at input " << input
                        << "." << (vc % _input_speedup)
                        << "." << endl;
                }
                _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
                iter->second.second = expanded_output;
            } else {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "Switch allocation failed for VC " << vc
                        << " at input " << input
                        << ": Granted to VC " << granted_vc << "." << endl;
                }
                iter->second.second = STALL_CROSSBAR_CONFLICT;
            }
        } else if(_spec_sw_allocator) {
            expanded_output = _spec_sw_allocator->OutputAssigned(expanded_input);
            if(expanded_output >= 0) {
                assert((expanded_output % _output_speedup) == (input % _output_speedup));
                if(_spec_mask_by_reqs &&
                        _sw_allocator->OutputHasRequests(expanded_output)) {
                    if(f->watch) {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                            << "Discarding speculative grant for VC " << vc
                            << " at input " << input
                            << "." << (vc % _input_speedup)
                            << " because output " << (expanded_output / _output_speedup)
                            << "." << (expanded_output % _output_speedup)
                            << " has non-speculative requests." << endl;
                    }
                    iter->second.second = STALL_CROSSBAR_CONFLICT;
                } else if(!_spec_mask_by_reqs &&
                        (_sw_allocator->InputAssigned(expanded_output) >= 0)) {
                    if(f->watch) {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                            << "Discarding speculative grant for VC " << vc
                            << " at input " << input
                            << "." << (vc % _input_speedup)
                            << " because output " << (expanded_output / _output_speedup)
                            << "." << (expanded_output % _output_speedup)
                            << " has a non-speculative grant." << endl;
                    }
                    iter->second.second = STALL_CROSSBAR_CONFLICT;
                } else {
                    int const granted_vc = _spec_sw_allocator->ReadRequest(expanded_input,
                            expanded_output);
                    if(granted_vc == vc) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Assigning output " << (expanded_output / _output_speedup)
                                << "." << (expanded_output % _output_speedup)
                                << " to VC " << vc
                                << " at input " << input
                                << "." << (vc % _input_speedup)
                                << "." << endl;
                        }
                        _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
                        iter->second.second = expanded_output;
                    } else {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Switch allocation failed for VC " << vc
                                << " at input " << input
                                << ": Granted to VC " << granted_vc << "." << endl;
                        }
                        iter->second.second = STALL_CROSSBAR_CONFLICT;
                    }
                }
            } else {

                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "Switch allocation failed for VC " << vc
                        << " at input " << input
                        << ": No output granted." << endl;
                }

                iter->second.second = STALL_CROSSBAR_CONFLICT;

            }
        } else {

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "Switch allocation failed for VC " << vc
                    << " at input " << input
                    << ": No output granted." << endl;
            }

            iter->second.second = STALL_CROSSBAR_CONFLICT;

        }
    }

    //mike: if the router is not speculative, the evaluate ends here.
    if(!_speculative && (_sw_alloc_delay <= 1)) {
        return;
    }

    //mike: for speculative output
    for(deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
            iter != _sw_alloc_vcs.end();
            ++iter) {

        int const time = iter->first;
        assert(time >= 0);
        if(GetSimTime() < time) {
            break;
        }

        assert(iter->second.second != -1);

        int const expanded_output = iter->second.second;

        if(expanded_output >= 0) {

            int const output = expanded_output / _output_speedup;
            assert((output >= 0) && (output < _outputs));

            BufferState const * const dest_buf = _next_buf[output];

            int const input = iter->second.first.first;
            assert((input >= 0) && (input < _inputs));
            assert((input % _output_speedup) == (expanded_output % _output_speedup));
            int const vc = iter->second.first.second;
            assert((vc >= 0) && (vc < _vcs));

            int const expanded_input = input * _input_speedup + vc % _input_speedup;
            assert(_switch_hold_vc[expanded_input] != vc);

            Buffer const * const cur_buf = _buf[input];
            assert(!cur_buf->Empty(vc));
            assert((cur_buf->GetState(vc) == VC::active) ||
                    (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

            Flit const * const f = cur_buf->FrontFlit(vc);
            assert(f);
            assert(f->vc == vc);

            if((_switch_hold_in[expanded_input] >= 0) ||
                    (_switch_hold_out[expanded_output] >= 0)) {
                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "Discarding grant from input " << input
                        << "." << (vc % _input_speedup)
                        << " to output " << output
                        << "." << (expanded_output % _output_speedup)
                        << " due to conflict with held connection at ";
                    if(_switch_hold_in[expanded_input] >= 0) {
                        *gWatchOut << "input";
                    }
                    if((_switch_hold_in[expanded_input] >= 0) &&
                            (_switch_hold_out[expanded_output] >= 0)) {
                        *gWatchOut << " and ";
                    }
                    if(_switch_hold_out[expanded_output] >= 0) {
                        *gWatchOut << "output";
                    }
                    *gWatchOut << "." << endl;
                }
                iter->second.second = STALL_CROSSBAR_CONFLICT;
            } else if(_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)) {

                assert(f->head);

                if(_vc_allocator) { // separate VC and switch allocators

                    int const output_and_vc = _vc_allocator->OutputAssigned(input*_vcs+vc);

                    if(output_and_vc < 0) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Discarding grant from input " << input
                                << "." << (vc % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << " due to misspeculation." << endl;
                        }
                        iter->second.second = -1; // stall is counted in VC allocation path!
                    } else if((output_and_vc / _vcs) != output) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Discarding grant from input " << input
                                << "." << (vc % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << " due to port mismatch between VC and switch allocator." << endl;
                        }
                        iter->second.second = STALL_BUFFER_CONFLICT; // count this case as if we had failed allocation
                    } else if(dest_buf->IsFullFor((output_and_vc % _vcs))) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Discarding grant from input " << input
                                << "." << (vc % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << " due to lack of credit." << endl;
                        }
                        iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
                    }

                } else { // VC allocation is piggybacked onto switch allocation

                    OutputSet const * const route_set = cur_buf->GetRouteSet(vc);
                    assert(route_set);

                    set<OutputSet::sSetElement> const setlist = route_set->GetSet();

                    bool busy = true;
                    bool full = true;
                    bool reserved = false;

                    assert(!_noq || (setlist.size() == 1));

                    for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
                            iset != setlist.end();
                            ++iset) {
                        if(iset->output_port == output) {

                            int vc_start;
                            int vc_end;

                            if(_noq && _noq_next_output_port[input][vc] >= 0) {
                                assert(!_routing_delay);
                                vc_start = _noq_next_vc_start[input][vc];
                                vc_end = _noq_next_vc_end[input][vc];
                            } else {
                                vc_start = iset->vc_start;
                                vc_end = iset->vc_end;
                            }
                            assert(vc_start >= 0 && vc_start < _vcs);
                            assert(vc_end >= 0 && vc_end < _vcs);
                            assert(vc_end >= vc_start);

                            for(int out_vc = vc_start; out_vc <= vc_end; ++out_vc) {
                                assert((out_vc >= 0) && (out_vc < _vcs));
                                if(dest_buf->IsAvailableFor(out_vc)) {
                                    busy = false;
                                    if(!dest_buf->IsFullFor(out_vc)) {
                                        full = false;
                                        break;
                                    } else if(!dest_buf->IsFull()) {
                                        reserved = true;
                                    }
                                }
                            }
                            if(!full) {
                                break;
                            }
                        }
                    }

                    if(busy) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Discarding grant from input " << input
                                << "." << (vc % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << " because no suitable output VC for piggyback allocation is available." << endl;
                        }
                        iter->second.second = STALL_BUFFER_BUSY;
                    } else if(full) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Discarding grant from input " << input
                                << "." << (vc % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << " because all suitable output VCs for piggyback allocation are full." << endl;
                        }
                        iter->second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
                    }

                }

            } else {
                assert(cur_buf->GetOutputPort(vc) == output);

                int const match_vc = cur_buf->GetOutputVC(vc);
                assert((match_vc >= 0) && (match_vc < _vcs));

                if(dest_buf->IsFullFor(match_vc)) {
                    if(f->watch) {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                            << "  Discarding grant from input " << input
                            << "." << (vc % _input_speedup)
                            << " to output " << output
                            << "." << (expanded_output % _output_speedup)
                            << " due to lack of credit." << endl;
                    }
                    iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
                }
            }
        }
    }
}

void IQEVCRouter::_SWAllocUpdate( )
{
    while(!_sw_alloc_vcs.empty()) {

        pair<int, pair<pair<int, int>, int> > const & item = _sw_alloc_vcs.front();

        int const time = item.first;
        if((time < 0) || (GetSimTime() < time)) {
            break;
        }
        assert(GetSimTime() == time);

        int const input = item.second.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = item.second.first.second;
        assert((vc >= 0) && (vc < _vcs));

        Buffer * const cur_buf = _buf[input];
        assert(!cur_buf->Empty(vc));
        assert((cur_buf->GetState(vc) == VC::active) ||
                (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

        Flit * const f = cur_buf->FrontFlit(vc);
        assert(f);
        assert(f->vc == vc);

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Completed switch allocation for VC " << vc
                << " at input " << input
                << " (front: " << f->id
                << ")." << endl;
        }

        int const expanded_output = item.second.second;

        if(expanded_output >= 0) {

            int const expanded_input = input * _input_speedup + vc % _input_speedup;

            //mike: make sure there is no held connection at this point, allocator will give priority to EVC
            assert(_switch_hold_vc[expanded_input] < 0);
            assert(_switch_hold_in[expanded_input] < 0);
            assert(_switch_hold_out[expanded_output] < 0);

            int const output = expanded_output / _output_speedup;
            assert((output >= 0) && (output < _outputs));

            BufferState * const dest_buf = _next_buf[output];

            int match_vc;

            if(!_vc_allocator && (cur_buf->GetState(vc) == VC::vc_alloc)) {

                assert(f->head);

                int const cl = f->cl;
                assert((cl >= 0) && (cl < _classes));


                int const vc_offset = _vc_rr_offset[output*_classes+cl];

                match_vc = -1;
                int match_prio = numeric_limits<int>::min();

                const OutputSet * route_set = cur_buf->GetRouteSet(vc);
                set<OutputSet::sSetElement> const setlist = route_set->GetSet();

                assert(!_noq || (setlist.size() == 1));

                for(set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
                        iset != setlist.end();
                        ++iset) {
                    if(iset->output_port == output) {

                        int vc_start;
                        int vc_end;

                        if(_noq && _noq_next_output_port[input][vc] >= 0) {
                            assert(!_routing_delay);
                            vc_start = _noq_next_vc_start[input][vc];
                            vc_end = _noq_next_vc_end[input][vc];
                        } else {
                            //vc_start = iset->vc_start;
                            //vc_end = iset->vc_end;

                            //jason: changing to accomodate evcs
                            if (f->cl == _evc_prioritized_class) {
                                vc_end = _vcs - 1;
                                vc_start = _vcs - _num_evcs;
                            } else {
                                vc_start = 0;
                                vc_end = _vcs - 1 - _num_evcs;
                            }
                        }
                        assert(vc_start >= 0 && vc_start < _vcs);
                        assert(vc_end >= 0 && vc_end < _vcs);
                        assert(vc_end >= vc_start);

                        for(int out_vc = vc_start; out_vc <= vc_end; ++out_vc) {
                            assert((out_vc >= 0) && (out_vc < _vcs));

                            int vc_prio = iset->pri;
                            if(_vc_prioritize_empty && !dest_buf->IsEmptyFor(out_vc)) {
                                assert(vc_prio >= 0);
                                vc_prio += numeric_limits<int>::min();
                            }

                            // FIXME: This check should probably be performed in Evaluate(),
                            // not Update(), as the latter can cause the outcome to depend on
                            // the order of evaluation!
                            if(dest_buf->IsAvailableFor(out_vc) &&
                                    !dest_buf->IsFullFor(out_vc) &&
                                    ((match_vc < 0) ||
                                     RoundRobinArbiter::Supersedes(out_vc, vc_prio,
                                         match_vc, match_prio,
                                         vc_offset, _vcs))) {
                                match_vc = out_vc;
                                match_prio = vc_prio;
                            }
                        }
                    }
                }
                assert(match_vc >= 0);

                if(f->watch) {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                        << "  Allocating VC " << match_vc
                        << " at output " << output
                        << " via piggyback VC allocation." << endl;
                }

                cur_buf->SetState(vc, VC::active);
                cur_buf->SetOutput(vc, output, match_vc);
                dest_buf->TakeBuffer(match_vc, input*_vcs + vc);

                _vc_rr_offset[output*_classes+cl] = (match_vc + 1) % _vcs;

            } else {

                assert(cur_buf->GetOutputPort(vc) == output);

                match_vc = cur_buf->GetOutputVC(vc);

            }
            assert((match_vc >= 0) && (match_vc < _vcs));

            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  Scheduling switch connection from input " << input
                    << "." << (vc % _input_speedup)
                    << " to output " << output
                    << "." << (expanded_output % _output_speedup)
                    << "." << endl;
            }

            cur_buf->RemoveFlit(vc);

#ifdef TRACK_FLOWS
            --_stored_flits[f->cl][input];
            if(f->tail) --_active_packets[f->cl][input];
#endif

            _bufferMonitor->read(input, f) ;

            f->hops++;
            f->vc = match_vc;

            if(!_routing_delay && f->head) {
                const FlitChannel * channel = _output_channels[output];
                const Router * router = channel->GetSink();
                if(router) {
                    if(_noq) {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Updating lookahead routing information for flit " << f->id
                                << " (NOQ)." << endl;
                        }
                        int next_output_port = _noq_next_output_port[input][vc];
                        assert(next_output_port >= 0);
                        _noq_next_output_port[input][vc] = -1;
                        int next_vc_start = _noq_next_vc_start[input][vc];
                        assert(next_vc_start >= 0 && next_vc_start < _vcs);
                        _noq_next_vc_start[input][vc] = -1;
                        int next_vc_end = _noq_next_vc_end[input][vc];
                        assert(next_vc_end >= 0 && next_vc_end < _vcs);
                        _noq_next_vc_end[input][vc] = -1;
                        f->la_route_set.Clear();
                        f->la_route_set.AddRange(next_output_port, next_vc_start, next_vc_end);
                    } else {
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Updating lookahead routing information for flit " << f->id
                                << "." << endl;
                        }
                        int in_channel = channel->GetSinkPort();
                        _rf(router, f, in_channel, &f->la_route_set, false);
                    }
                } else {
                    f->la_route_set.Clear();
                }
            }

#ifdef TRACK_FLOWS
            ++_outstanding_credits[f->cl][output];
            _outstanding_classes[output][f->vc].push(f->cl);
#endif

            dest_buf->SendingFlit(f);

            _crossbar_flits.push_back(make_pair(-1, make_pair(f, make_pair(expanded_input, expanded_output))));

            if(_out_queue_credits.count(input) == 0) {
                _out_queue_credits.insert(make_pair(input, Credit::New()));
            }
            _out_queue_credits.find(input)->second->vc.insert(vc);

            if(cur_buf->Empty(vc)) {
                if(f->tail) {
                    cur_buf->SetState(vc, VC::idle);
                }
            } else {
                Flit * const nf = cur_buf->FrontFlit(vc);
                assert(nf);
                assert(nf->vc == vc);
                if(f->tail) {
                    assert(nf->head);
                    if(_routing_delay) {
                        cur_buf->SetState(vc, VC::routing);
                        _route_vcs.push_back(make_pair(-1, item.second.first));
                    } else {
                        if(nf->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Using precomputed lookahead routing information for VC " << vc
                                << " at input " << input
                                << " (front: " << nf->id
                                << ")." << endl;
                        }
                        cur_buf->SetRouteSet(vc, &nf->la_route_set);
                        cur_buf->SetState(vc, VC::vc_alloc);
                        if(_speculative) {
                            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                            -1)));
                        }
                        if(_vc_allocator) {
                            _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                            -1)));
                        }
                        if(_noq) {
                            _UpdateNOQ(input, vc, nf);
                        }
                    }
                } else {
                    //mike: this is where the switch holding is setup
                    //mike: so now the connection has been established, updated switch_held list for
                    //these connections. Only hold switch for EVC
                    if((_hold_switch_for_packet) || ((_hold_switch_for_evc_packet) && _evc && (f->cl == _evc_prioritized_class))) { //
                        if(f->watch) {
                            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                                << "Setting up switch hold for VC " << vc
                                << " at input " << input
                                << "." << (expanded_input % _input_speedup)
                                << " to output " << output
                                << "." << (expanded_output % _output_speedup)
                                << "." << endl;
                        }
                        //printf("holding: %d/%d->%d, pri:%d\n", expanded_input, vc, expanded_output,f->pri);
                        _switch_hold_vc[expanded_input] = vc;
                        _switch_hold_in[expanded_input] = expanded_output;
                        _switch_hold_out[expanded_output] = expanded_input;
                        _sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                        -1)));
                    } else {
                        _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                        -1)));
                    }
                }
            }
        } else {
            if(f->watch) {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "  No output port allocated." << endl;
            }

#ifdef TRACK_STALLS
            assert((expanded_output == -1) || // for stalls that are accounted for in VC allocation path
                    (expanded_output == STALL_BUFFER_BUSY) ||
                    (expanded_output == STALL_BUFFER_CONFLICT) ||
                    (expanded_output == STALL_BUFFER_FULL) ||
                    (expanded_output == STALL_BUFFER_RESERVED) ||
                    (expanded_output == STALL_CROSSBAR_CONFLICT));
            if(expanded_output == STALL_BUFFER_BUSY) {
                ++_buffer_busy_stalls[f->cl];
            } else if(expanded_output == STALL_BUFFER_CONFLICT) {
                ++_buffer_conflict_stalls[f->cl];
            } else if(expanded_output == STALL_BUFFER_FULL) {
                ++_buffer_full_stalls[f->cl];
            } else if(expanded_output == STALL_BUFFER_RESERVED) {
                ++_buffer_reserved_stalls[f->cl];
            } else if(expanded_output == STALL_CROSSBAR_CONFLICT) {
                ++_crossbar_conflict_stalls[f->cl];
            }
#endif

            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
        }
        _sw_alloc_vcs.pop_front();
    }
}


//------------------------------------------------------------------------------
// switch traversal
//------------------------------------------------------------------------------

void IQEVCRouter::_SwitchEvaluate( )
{
    for(deque<pair<int, pair<Flit *, pair<int, int> > > >::iterator iter = _crossbar_flits.begin();
            iter != _crossbar_flits.end();
            ++iter) {

        int const time = iter->first;
        if(time >= 0) {
            break;
        }
        iter->first = GetSimTime() + _crossbar_delay - 1;

        Flit const * const f = iter->second.first;
        assert(f);

        int const expanded_input = iter->second.second.first;
        int const expanded_output = iter->second.second.second;

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Beginning crossbar traversal for flit " << f->id
                << " from input " << (expanded_input / _input_speedup)
                << "." << (expanded_input % _input_speedup)
                << " to output " << (expanded_output / _output_speedup)
                << "." << (expanded_output % _output_speedup)
                << "." << endl;
        }
    }
}

void IQEVCRouter::_SwitchUpdate( )
{
    while(!_crossbar_flits.empty()) {

        pair<int, pair<Flit *, pair<int, int> > > const & item = _crossbar_flits.front();

        int const time = item.first;
        if((time < 0) || (GetSimTime() < time)) {
            break;
        }
        assert(GetSimTime() == time);

        Flit * const f = item.second.first;
        assert(f);

        int const expanded_input = item.second.second.first;
        int const input = expanded_input / _input_speedup;
        assert((input >= 0) && (input < _inputs));
        int const expanded_output = item.second.second.second;
        int const output = expanded_output / _output_speedup;
        assert((output >= 0) && (output < _outputs));

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Completed crossbar traversal for flit " << f->id
                << " from input " << input
                << "." << (expanded_input % _input_speedup)
                << " to output " << output
                << "." << (expanded_output % _output_speedup)
                << "." << endl;
        }
        _switchMonitor->traversal(input, output, f) ;

        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Buffering flit " << f->id
                << " at output " << output
                << "." << endl;
        }
        _output_buffer[output].push(f);
        //the output buffer size isn't precise due to flits in flight
        //but there is a maximum bound based on output speed up and ST traversal
        assert(_output_buffer[output].size()<=(size_t)_output_buffer_size+ _crossbar_delay* _output_speedup+( _output_speedup-1) ||_output_buffer_size==-1);
        _crossbar_flits.pop_front();
    }
}


//------------------------------------------------------------------------------
// output queuing
//------------------------------------------------------------------------------

void IQEVCRouter::_OutputQueuing( )
{
    for(map<int, Credit *>::const_iterator iter = _out_queue_credits.begin();
            iter != _out_queue_credits.end();
            ++iter) {

        int const input = iter->first;
        assert((input >= 0) && (input < _inputs));

        Credit * const c = iter->second;
        assert(c);
        assert(!c->vc.empty());

        _credit_buffer[input].push(c);
    }
    _out_queue_credits.clear();
}

//------------------------------------------------------------------------------
// write outputs
//------------------------------------------------------------------------------

void IQEVCRouter::_SendFlits( )
{
    for ( int output = 0; output < _outputs; ++output ) {
        if ( !_output_buffer[output].empty( ) ) {
            Flit * const f = _output_buffer[output].front( );
            assert(f);
            _output_buffer[output].pop( );

#ifdef TRACK_FLOWS
            ++_sent_flits[f->cl][output];
#endif

            if(f->watch)
                *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                    << "Sending flit " << f->id
                    << " to channel at output " << output
                    << "." << endl;
            if(gTrace) {
                cout << "Outport " << output << endl << "Stop Mark" << endl;
            }
            _output_channels[output]->Send( f );
        }
    }
}

void IQEVCRouter::_SendCredits( )
{
    for ( int input = 0; input < _inputs; ++input ) {
        if ( !_credit_buffer[input].empty( ) ) {
            Credit * const c = _credit_buffer[input].front( );
            assert(c);
            _credit_buffer[input].pop( );
            _input_credits[input]->Send( c );
        }
    }
}


//------------------------------------------------------------------------------
// misc.
//------------------------------------------------------------------------------

void IQEVCRouter::Display( ostream & os ) const
{
    for ( int input = 0; input < _inputs; ++input ) {
        _buf[input]->Display( os );
    }
}

int IQEVCRouter::GetUsedCredit(int o) const
{
    assert((o >= 0) && (o < _outputs));
    BufferState const * const dest_buf = _next_buf[o];
    return dest_buf->Occupancy();
}

int IQEVCRouter::GetBufferOccupancy(int i) const {
    assert(i >= 0 && i < _inputs);
    return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int IQEVCRouter::GetUsedCreditForClass(int output, int cl) const
{
    assert((output >= 0) && (output < _outputs));
    BufferState const * const dest_buf = _next_buf[output];
    return dest_buf->OccupancyForClass(cl);
}

int IQEVCRouter::GetBufferOccupancyForClass(int input, int cl) const
{
    assert((input >= 0) && (input < _inputs));
    return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> IQEVCRouter::UsedCredits() const
{
    vector<int> result(_outputs*_vcs);
    for(int o = 0; o < _outputs; ++o) {
        for(int v = 0; v < _vcs; ++v) {
            result[o*_vcs+v] = _next_buf[o]->OccupancyFor(v);
        }
    }
    return result;
}

vector<int> IQEVCRouter::FreeCredits() const
{
    vector<int> result(_outputs*_vcs);
    for(int o = 0; o < _outputs; ++o) {
        for(int v = 0; v < _vcs; ++v) {
            result[o*_vcs+v] = _next_buf[o]->AvailableFor(v);
        }
    }
    return result;
}

vector<int> IQEVCRouter::MaxCredits() const
{
    vector<int> result(_outputs*_vcs);
    for(int o = 0; o < _outputs; ++o) {
        for(int v = 0; v < _vcs; ++v) {
            result[o*_vcs+v] = _next_buf[o]->LimitFor(v);
        }
    }
    return result;
}

void IQEVCRouter::_UpdateNOQ(int input, int vc, Flit const * f) {
    assert(!_routing_delay);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);
    set<OutputSet::sSetElement> sl = f->la_route_set.GetSet();
    assert(sl.size() == 1);
    int out_port = sl.begin()->output_port;
    const FlitChannel * channel = _output_channels[out_port];
    const Router * router = channel->GetSink();
    if(router) {
        int in_channel = channel->GetSinkPort();
        OutputSet nos;
        _rf(router, f, in_channel, &nos, false);
        sl = nos.GetSet();
        assert(sl.size() == 1);
        OutputSet::sSetElement const & se = *sl.begin();
        int next_output_port = se.output_port;
        assert(next_output_port >= 0);
        assert(_noq_next_output_port[input][vc] < 0);
        _noq_next_output_port[input][vc] = next_output_port;
        int next_vc_count = (se.vc_end - se.vc_start + 1) / router->NumOutputs();
        int next_vc_start = se.vc_start + next_output_port * next_vc_count;
        assert(next_vc_start >= 0 && next_vc_start < _vcs);
        assert(_noq_next_vc_start[input][vc] < 0);
        _noq_next_vc_start[input][vc] = next_vc_start;
        int next_vc_end = se.vc_start + (next_output_port + 1) * next_vc_count - 1;
        assert(next_vc_end >= 0 && next_vc_end < _vcs);
        assert(_noq_next_vc_end[input][vc] < 0);
        _noq_next_vc_end[input][vc] = next_vc_end;
        assert(next_vc_start <= next_vc_end);
        if(f->watch) {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | "
                << "Computing lookahead routing information for flit " << f->id
                << " (NOQ)." << endl;
        }
    }
}
