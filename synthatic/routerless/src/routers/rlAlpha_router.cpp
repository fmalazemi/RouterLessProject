// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
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


/*
The implementation of RouterLess
interface.
*/



#include "rlAlpha_router.hpp"

#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cassert>
#include <limits>
#include <algorithm>

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

RLALPHARouter::RLALPHARouter( Configuration const & config, Module *parent,
		    string const & name, int id, int inputs, int outputs )
: Router( config, parent, name, id, inputs, outputs ), _active(false)
{

	useSpaceWhileStall = (config.GetInt("use_space_while_stall")> 0);
	_ejectors = config.GetInt("ejectors") ;
	assert(_ejectors > 0);
	// the number of ejectors must not exceeds number of rings in the interface
	_ejectors = (_ejectors <= _inputs-1)? _ejectors : _inputs-1 ;
  ejector.resize(_ejectors);


	for(int e = 0; e < _ejectors; e++){
		ejector[e].inUse     = false ;
		ejector[e].reset     = false ;
		ejector[e].ringID    = -1 ;
		ejector[e].input 	   = -1;
    ejector[e].pid       = -1;
	}

  injector.ringID     = -1;
  injector.pid        = -1;
  injector.inUse      = false;
  injector.output     = -1;
  injector.reset      = false ;


  ringState.resize(_inputs);
	for(int input = 0; input < _inputs; input++){

    ringState[input].isEjecting  = false ;
    ringState[input].ejectorID     = -1;
    ringState[input].isInjecting = false ;
    ringState[input].isForward   = false ;

	}

  _vcs  = config.GetInt( "num_vcs" );


  // Routing
  string const rf = config.GetStr("routing_function") + "_" + config.GetStr("topology");

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
		_next_buf[j]->resetBufferSize(infinite);
		//IF a buffer overflow occured, it will be detected by _buf. So it is safe
		//to set buffer size to infinite .
  }


	//unlimited injection and ejection buffer size
	_buf[0]->resetBufferSize(infinite);
	_next_buf[0]->resetBufferSize(infinite);

	cout<<" --- "<<GetID()<<" has BufState size = "<<_next_buf[0]->LimitFor(0)<<endl;


	_output_buffer_size = config.GetInt("output_buffer_size");
	_output_buffer.resize(_outputs);
	_credit_buffer.resize(_inputs);


  _bufferMonitor = new BufferMonitor(inputs, _classes);
  _switchMonitor = new SwitchMonitor(inputs, outputs, _classes);

}

RLALPHARouter::~RLALPHARouter( )
{

  if(gPrintActivity ) {
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

  delete _bufferMonitor;
  delete _switchMonitor;
}

void RLALPHARouter::AddInputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
	channel->setRLMode();
	backchannel->setRLMode();
	_ringInput.insert(make_pair(channel->GetRingID(), _input_channels.size()) );
	Router::AddInputChannel(channel, backchannel);
}



void RLALPHARouter::AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
	channel->setRLMode();
	backchannel->setRLMode();
	_ringOutput.insert(make_pair(channel->GetRingID(), _output_channels.size()) );
  _next_buf[_output_channels.size()]->SetMinLatency(0);
  Router::AddOutputChannel(channel, backchannel);
}

void RLALPHARouter::ReadInputs( )
{
	_ReceiveFlits( );
  _ReceiveCredits( );

}


void RLALPHARouter::printBuffers(){
	cout<<GetSimTime()<<": At Router "<<GetID()<<endl;
for(int i = 0; i < _inputs; i++){
	cout<<"      buf["<<i<<"] = ";
		_buf[i]->Display2();
	}

		for(int i = 0; i < _outputs; i++){

			cout<<"      BufState["<<i<<"] = "<<_next_buf[i]->Occupancy()<<endl;

		}

}

void RLALPHARouter::_InternalStep()
{

 	_InputQueuing() ;
	_EjectionModule() ;

 	_InputModule() ;

 	_InjectionModule() ;

	_RouteTraffic() ;

	_resetStatus() ;

  _OutputQueuing( );


  _bufferMonitor->cycle( );
  _switchMonitor->cycle( );



}

void RLALPHARouter::WriteOutputs( )
{
  _SendFlits( );
  _SendCredits( );
}


//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool RLALPHARouter::_ReceiveFlits( )
{
  bool activity = false;
  for(int input = 0; input < _inputs; ++input) {
    Flit * const f = _input_channels[input]->Receive();
    if(f) {

      _in_queue_flits.insert(make_pair(input, f));

      activity = true;
    }
  }
  return activity;
}

bool RLALPHARouter::_ReceiveCredits( )
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

void RLALPHARouter::_InputQueuing( )
{
  for(map<int, Flit *>::const_iterator iter = _in_queue_flits.begin();
      iter != _in_queue_flits.end();
      ++iter) {

    int const input = iter->first;
    assert((input >= 0) && (input < _inputs));

    Flit * const f = iter->second;
    assert(f);
		if(input == 0){
				f->creationTime = GetSimTime();
        //TODO: CREATION TIME IS THE SAME FOR THE WHOLE PACKET
		}

    int const vc = f->vc;


    assert(vc == 0);

    Buffer * const cur_buf = _buf[input];

    cur_buf->AddFlit(vc, f);
    _bufferMonitor->write(input, f) ;


	}

  _in_queue_flits.clear();

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


    dest_buf->ProcessCredit(c);
    c->Free();
    _proc_credits.pop_front();
  }
}




int RLALPHARouter::_oldestPacket(){

  int maxAge = -1 ;
  int maxInput = -1;
  int curTime = GetSimTime();
  int id = GetID();


  for(int input = 1; input < _inputs; input++){


    if(_buf[input]->Empty(0)) continue;

    Flit * const f = _buf[input]->FrontFlit(0);
		assert(f);

		if(f->dest == id && f->head && ! ringState[input].isEjecting){
			int age = curTime - f->creationTime;
			if(age > maxAge){
				maxAge = age;
				maxInput = input;
			}
		}



  }

  return maxInput ;


}


void RLALPHARouter::_resetStatus(){

  for(int e = 0; e < _ejectors; e++){
    if(ejector[e].reset){
      int input = ejector[e].input;
      ringState[input].isEjecting  = false ;
      ringState[input].ejectorID   = -1;


      ejector[e].inUse     = false ;
      ejector[e].reset     = false ;
      ejector[e].input 	   = -1;
      ejector[e].pid       = -1;
      ejector[e].ringID    = -1 ;


    }
  }

  if(injector.reset){
    int output = injector.output;
    ringState[output].isInjecting = false ;
    injector.ringID = -1;
    injector.output = -1;
    injector.pid    = -1  ;
    injector.inUse = false  ;
    injector.reset = false  ;
  }


  for(int i = 1; i < _inputs; i++){
      if(ringState[i].resetForward){
        ringState[i].isForward = false ;
        ringState[i].resetForward = false ;
      }
  }

}




void RLALPHARouter::_EjectionModule(){


  for(int e = 0; e < _ejectors; e++){
    if(ejector[e].inUse){
      int input = ejector[e].input ;
      int rID   = ejector[e].ringID ;
      int pid   = ejector[e].pid ;
      assert(input >= 1 && input < _inputs);
      assert( ! _buf[input]->Empty(0));
      Flit * const f = _buf[input]->FrontFlit(0);
      assert( ! f->head );
      assert( f->ringID == rID);
      assert(f->pid == pid);
      f->arrivalTime = GetSimTime() ;
      input_to_output.push_back(make_pair(input, 0));
      if(f->tail){
        ejector[e].reset = true ;
      }
    }else{
      int input = _oldestPacket();
      if(input != -1){
        assert(input >= 1 && input < _inputs);
        assert( ! _buf[input]->Empty(0) );
        assert( ! ringState[input].isEjecting) ;
        Flit * const f = _buf[input]->FrontFlit(0);
        assert(f);
        f->arrivalTime = GetSimTime();
        ringState[input].isEjecting = true;
        ringState[input].ejectorID  = e;
        ejector[e].input  = input;
        ejector[e].inUse  = true;
        ejector[e].reset  = false ;
        ejector[e].pid    = f->pid;
        ejector[e].ringID = f->ringID ;

        input_to_output.push_back(make_pair(input, 0));
        if(f->tail){
          ejector[e].reset = true ;
        }





      }



    }


  }

  /*




  for(int e = 0; e < _ejectors; e++){
    if(ejector[e].inUse){

      int input = ejector[e].input;
      assert(input >= 1 && input < _inputs);
      assert( ! _buf[input]->Empty(0));
      Flit * const f = _buf[input]->FrontFlit(0);
      assert( ! f->head ) ;
      int rID = ejector[e].ringID;
      assert(f->ringID == rID);

      assert(f->pid == ejector[e].pid);
      assert(ringState[input].isEjecting);

      f->arrivalTime = GetSimTime() ;
      input_to_output.push_back(make_pair(input, 0));


      if(f->tail){
        ejector[e].reset = true;
      }
    }else{

      int input = _oldestPacket();

      if(input != -1){
        assert( ! _buf[input]->Empty(0) );
        Flit * const f = _buf[input]->FrontFlit(0);
        assert(f);
        assert(_ringInput[f->ringID] == input );
        assert( ! ringState[input].isEjecting) ;

        ringState[input].isEjecting = true;
        ringState[input].ejectorID  = e;
        ejector[e].input = input;
        ejector[e].inUse = true;
        ejector[e].reset = false ;
        ejector[e].pid = f->pid;
        ejector[e].ringID = f->ringID ;

        f->arrivalTime = GetSimTime();


        input_to_output.push_back(make_pair(input, 0));

        if(f->tail) {
          ejector[e].reset = true;
        }
      }
    }
  }*/

}




void RLALPHARouter::_InputModule(){
	//ejectionModule must execute before this function
	//injectionModule must execute after this function
	//Here we decide whether we should forward a packet or stall.

	int id = GetID() ;

  for(int input = 1; input < _inputs; input++){

    Buffer * const cur_buf = _buf[input];
    if(ringState[input].isEjecting || ringState[input].isInjecting){

      //there is nothing to do

    }else if(ringState[input].isForward){

      assert( ! cur_buf->Empty(0) );
      Flit * const f = cur_buf->FrontFlit(0);
      assert(f);
      int rID = f->ringID ;
      int output = _ringOutput[rID] ;
      assert(input == output);
      assert( ! f->head ) ;
      assert( ! ringState[input].resetForward );
      input_to_output.push_back(make_pair(input, output));
      if(f->tail){
        ringState[input].resetForward = true;
      }
    }else if( ! cur_buf->Empty(0)){
      Flit * const f = cur_buf->FrontFlit(0);
      assert(f);
      assert(f->head);
      int rID = f->ringID ;
      int output = _ringOutput[rID] ;
      assert( input == output) ;
      assert( !ringState[input].resetForward );
      if( (f->dest == id && cur_buf->Full() )  || f->dest != id){
        ringState[input].isForward = true;
        input_to_output.push_back(make_pair(input, output));
        if(f->tail){
          ringState[input].resetForward = true;
        }
      }
    }
  }

}



bool RLALPHARouter::_canInjection(int input, int pSize/*packet Size */){
	if( ! useSpaceWhileStall) {
		return _buf[input]->Empty(0);
	}
	if(ringState[input].isForward || ringState[input].isInjecting){

		return false;
	}

	if(_buf[input]->Empty(0)) {

		return true;
	}

	int available = _buf[input]->getSize() - _buf[input]->GetOccupancy();

	return available >= pSize ;

}

void RLALPHARouter::_InjectionModule(){
	Buffer * const cur_buf = _buf[0]; // input from node
	if( cur_buf->Empty(0) ){
		return ; //nothing to do.
	}

	Flit * const f = _buf[0]->FrontFlit(0);
	assert(f);
	assert(f->src == GetID());

	/*if(f->src == f->dest){

		injector.inUse    = true ;
		injector.pid      = f->pid;
		injector.ringID   = -1;
		injector.output = 0;
		f->arrivalTime = GetSimTime();


		f->ringID = -1 ;
		f->injectionTime = GetSimTime();

		input_to_output.push_back(make_pair(0, 0) );
		return ;
	}*/

	if( ! f->head ){

		assert(injector.inUse);
		//assert(ringState[injector.output].isInjecting);
		assert(injector.pid == f->pid) ;
		f->ringID = injector.ringID;
		f->injectionTime = GetSimTime();
		if(injector.output == 0){
			f->arrivalTime = GetSimTime();
		}

		input_to_output.push_back(make_pair(0, injector.output) );

		if(f->tail){
			injector.reset = true;

		}



	}else{

		assert( ! injector.inUse );
		/*
		// to shuffle ring selection
		vector<int> dd ;
		dd.resize(_RL[f->dest].size());

		for(int zz = 0; zz < _RL[f->dest].size(); zz++){
 			dd[zz] = _RL[f->dest][zz];
 		}
		random_shuffle(dd.begin(), dd.end());
		*/


		for(int r = 0; r < _RL[f->dest].size(); r++){
			int rID = _RL[f->dest][r]; //for ring shuffle dd[r];//
 			int input = _ringInput[rID];

			if( _canInjection(input, f->pSize) ){

				injector.inUse    = true ;
				injector.pid      = f->pid;
				injector.ringID   = rID;
				injector.output = _ringOutput[rID];
				assert(injector.output == input);

				f->ringID = rID ;
				f->injectionTime = GetSimTime();

				ringState[input].isInjecting = true;

				input_to_output.push_back(make_pair(0, injector.output) );
				if(f->tail){
					injector.reset = true  ;
					ringState[input].resetForward = true ;

				}

				return ; //we have found a ring to inject, done here.
			}

		}
	}
	//All rings that reach f->dest are busy.

}



//------------------------------------------------------------------------------
// routing
//------------------------------------------------------------------------------


void RLALPHARouter::_RouteTraffic(){
	//There is no need for any validity checks here, We had done enough above.
	//all traffic is routed here except for ejection traffic
	//We still remove all traffic from buffers here

 	for(int i = 0; i < input_to_output.size(); i++){

		int input = input_to_output[i].first;
		int output = input_to_output[i].second;

		Buffer * const cur_buf = _buf[input];

		assert( ! cur_buf->Empty(0) ) ;
		Flit * const f = cur_buf->FrontFlit(0);
		assert(f);

		if(input == 0){
			//assert(output == injector.output);
		}


		_output_buffer[output].push(f);


		_bufferMonitor->read(i, f) ;

		f->hops++;
		BufferState * const dest_buf = _next_buf[output];

		if(f->head){
			dest_buf->TakeBuffer(0, input*1 + 0);
		}
		cur_buf->RemoveFlit(0);



		if(dest_buf->IsFullFor(0)){
			cout<<"At router "<<GetID()<<endl;
			cout<<"full Input = "<<input<<", output = "<<output<<endl;
			cout<<"BuffStateSize = "<<dest_buf->Occupancy()<<endl;
			cout<<"Packet ID = "<<f->pid<<endl;
		}
		dest_buf->SendingFlit(f);
		if(_out_queue_credits.count(input) == 0) {
				_out_queue_credits.insert(make_pair(input, Credit::New()));
			}
			_out_queue_credits.find(input)->second->vc.insert(0);


			_switchMonitor->traversal(i, output, f) ;

		}
	input_to_output.clear();

}


//------------------------------------------------------------------------------
// output queuing
//------------------------------------------------------------------------------

void RLALPHARouter::_OutputQueuing( )
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

void RLALPHARouter::_SendFlits( )
{


  for ( int output = 0; output < _outputs; ++output ) {


    if ( !_output_buffer[output].empty( ) ) {

      Flit * const f = _output_buffer[output].front( );
      assert(f);


			if(f->pid == 15){
				//cout<<GetSimTime()<<": PID = 15 is at "<<GetID()<<", output channel = "<<output<<" ------ SendFlits"<<endl;

			}


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

void RLALPHARouter::_SendCredits( )
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

void RLALPHARouter::Display( ostream & os ) const
{
  for ( int input = 0; input < _inputs; ++input ) {
    _buf[input]->Display( os );
  }
}

int RLALPHARouter::GetUsedCredit(int o) const
{
  assert((o >= 0) && (o < _outputs));
  BufferState const * const dest_buf = _next_buf[o];
  return dest_buf->Occupancy();
}

int RLALPHARouter::GetBufferOccupancy(int i) const {
  assert(i >= 0 && i < _inputs);
  return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int RLALPHARouter::GetUsedCreditForClass(int output, int cl) const
{
  assert((output >= 0) && (output < _outputs));
  BufferState const * const dest_buf = _next_buf[output];
  return dest_buf->OccupancyForClass(cl);
}

int RLALPHARouter::GetBufferOccupancyForClass(int input, int cl) const
{
  assert((input >= 0) && (input < _inputs));
  return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> RLALPHARouter::UsedCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->OccupancyFor(v);
    }
  }
  return result;
}

vector<int> RLALPHARouter::FreeCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->AvailableFor(v);
    }
  }
  return result;
}

vector<int> RLALPHARouter::MaxCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->LimitFor(v);
    }
  }
  return result;
}
