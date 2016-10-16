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



#include "rl_router.hpp"

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

RLRouter::RLRouter( Configuration const & config, Module *parent,
		    string const & name, int id, int inputs, int outputs )
: Router( config, parent, name, id, inputs, outputs ), _active(false)
{

	useSpaceWhileStall = (config.GetInt("use_space_while_stall")> 0);
	_ejectors = config.GetInt("ejectors") ;
	assert(_ejectors > 0);
	// the number of ejectors must not exceeds number of rings in the interface
	_ejectors = (_ejectors <= _inputs-1)? _ejectors : _inputs-1 ;
	ejectorInUse.resize(_ejectors);
  ejectionPID.resize(_ejectors);
  ejectorReset.resize(_ejectors);
	ejectionRing.resize(_ejectors) ;
	ejectionInput.resize(_ejectors);

	for(int e = 0; e < _ejectors; e++){
		ejectorInUse[e]  = false ;
		ejectorReset[e]  = false ;
		ejectionRing[e]  = -1 ;
		ejectionPID[e] 	 = -1;
		ejectionInput[e] = -1;
	}

	injectionRing = -1 ;
	injectionPID = -1;
	injectionOutput = -1 ;
	injectionReset = false ;
	injectionInUse = false ;

	ringStatus.resize(_inputs);
	ringReset.resize(_inputs);
	ringEjecting.resize(_inputs);

	for(int input = 0; input < _inputs; input++){
		ringStatus[input] = IDLE;
		ringReset[input] = false ;
		ringEjecting[input] = false ;
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

RLRouter::~RLRouter( )
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

  delete _bufferMonitor;
  delete _switchMonitor;
}

void RLRouter::AddInputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
	channel->setRLMode();
	backchannel->setRLMode();
	_ringInput.insert(make_pair(channel->GetRingID(), _input_channels.size()) );

	Router::AddInputChannel(channel, backchannel);
}



void RLRouter::AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
	channel->setRLMode();
	backchannel->setRLMode();
	_ringOutput.insert(make_pair(channel->GetRingID(), _output_channels.size()) );
  _next_buf[_output_channels.size()]->SetMinLatency(0);
  Router::AddOutputChannel(channel, backchannel);
}

void RLRouter::ReadInputs( )
{
	_ReceiveFlits( );
  _ReceiveCredits( );

}


void RLRouter::printBuffers(){
	cout<<GetSimTime()<<": At Router "<<GetID()<<endl;
for(int i = 0; i < _inputs; i++){
	cout<<"      buf["<<i<<"] = ";
		_buf[i]->Display2();
	}

		for(int i = 0; i < _outputs; i++){

			cout<<"      BufState["<<i<<"] = "<<_next_buf[i]->Occupancy()<<endl;

		}

}

void RLRouter::_InternalStep()
{

 	_InputQueuing( );
	_EjectionModule();
 	_InputModule() ;
 	_InjectionModule();
	_RouteTraffic();
	_resetStatus() ;
  _OutputQueuing( );

  _bufferMonitor->cycle( );
  _switchMonitor->cycle( );



}

void RLRouter::WriteOutputs( )
{
  _SendFlits( );
  _SendCredits( );
}


//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool RLRouter::_ReceiveFlits( )
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

bool RLRouter::_ReceiveCredits( )
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

void RLRouter::_InputQueuing( )
{
  for(map<int, Flit *>::const_iterator iter = _in_queue_flits.begin();
      iter != _in_queue_flits.end();
      ++iter) {

    int const input = iter->first;
    assert((input >= 0) && (input < _inputs));

    Flit * const f = iter->second;
    assert(f);
		if(input == 0){
				f->creationTime = GetSimTime();//TODO: CREATION TIME IS THE SAME FOR THE WHOLE PACKET
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




int RLRouter::_oldestPacket(){

	int maxAge = -1;
	int maxInput = -1;
	int curTime = GetSimTime() ;
	int id = GetID() ;
	for(int input = 1; input < _inputs; input++){//ignore  core buf (_buf[0])
		if(_buf[input]->Empty(0)) continue;

		Flit * const f = _buf[input]->FrontFlit(0);
		assert(f);

		if(f->dest == id && f->head && ringStatus[input] == IDLE){
			int age = curTime - f->creationTime;
			if(age > maxAge){
				maxAge = age;
				maxInput = input;
			}
		}
	}

	//If maxInput == -1, then all the current flits at all buffers are not dest to
	// this router.

	return maxInput ;
}


void RLRouter::_resetStatus(){

	for(int e = 0; e < _ejectors; e++){
		if(ejectorReset[e]){
			ejectorReset[e] = false ;
			ejectorInUse[e] = false ;
	//		ringEjecting[ejectionRing] = false ;
			ejectionRing[e] = -1;
			ejectionPID[e] = -1;
			ejectionInput[e] = -1;

		}
	}
	for(int input = 0; input < _inputs; input++){
		if(ringReset[input]){
			ringStatus[input] = IDLE;
			ringReset[input] = false ;
		}
	}
	if(injectionReset){
		injectionPID = -1;
		injectionRing = -1 ;
		injectionOutput = -1;
		injectionReset = false ;
		injectionInUse = false ;
	}
}




void RLRouter::_EjectionModule(){

	int x = 0;
	for(int e = 0; e < _ejectors; e++){

		if(ejectorInUse[e]){
			x++;

			int input = ejectionInput[e];
			assert( input >= 1 && input < _inputs );
			assert( ! _buf[input]->Empty(0) );
			Flit * const f = _buf[input]->FrontFlit(0);
			assert( ! f->head ) ;

			int rID = ejectionRing[e] ;
			assert(f->ringID == rID);

			assert( f->pid == ejectionPID[e] );
			assert( ! ejectorReset[e] ) ;
			assert(ringStatus[input] == EJECTION);
			assert(_ringInput[rID] == input);

			f->arrivalTime = GetSimTime();

			input_to_output.push_back(make_pair(input, 0));

			//ejection_buffer[e].push_back(f);



			if(f->tail){
				//In the next cycle, ejector e can be used by any ring.
				ejectorReset[e] = true;
				ringReset[input] = true;
			}

		}else{


			int input = _oldestPacket() ;//input link where the oldest packet is.
			if(input != -1){
				assert( ! _buf[input]->Empty(0) );
				Flit * const f = _buf[input]->FrontFlit(0);
				assert(f);
				assert(_ringInput[f->ringID] == input );
				assert(ringStatus[input] == IDLE) ;


				ringStatus[input] = EJECTION ;
				ejectionInput[e] = input;
				ejectorReset[e] = false ;
				ejectorInUse[e] = true ;
				ejectionPID[e] = f->pid;
				ejectionRing[e] = f->ringID ;

				f->arrivalTime = GetSimTime();


				input_to_output.push_back(make_pair(input, 0));

			//	ejection_buffer[e].push_back(f);

				if(f->tail) {
					ejectorReset[e] = true;
					ringReset[input] = true;
				}
				x++;
			}


		}
	}
	if(x > 5)
	cout<<GetID()<<". has "<<x<<" ejectors active"<<endl;

}
int RLRouter::_getEjectorID(int r){
	//finds an ejector that uses the ring r for ejection.
	for(int e = 0; e < _ejectors; e++){
		if(ejectionRing[e] == r){
			assert(ejectorInUse[e]);
			return e;
		}
	}
	return -1;

}


void RLRouter::_InputModule(){
	//ejectionModule must execute before this function
	//injectionModule must execute after this function
	//Here we decide whether we should forward a packet or stall.

	int id = GetID() ;

	for(int input = 1; input < _inputs; input++){
		Buffer * const cur_buf = _buf[input];

		if(ringStatus[input] == INJECTION || ringStatus[input] == EJECTION) {
			if(ringStatus[input] == INJECTION){
				//assert( ! cur_buf->Full() );
			}else{
				//assert( ! cur_buf->Empty(0) );
			}

			//extra validity checks
			/*
			if(ringStatus[input] == INJECTION){
				assert(f->head);
			}else if(ringStatus[input] == EJECTION){
				assert(! cur_buf->Empty(0) ) ;
				Flit * const f = cur_buf->FrontFlit(0);
				assert(f);
				assert(f->dest == GetID());
				int e = _getEjectorID(f->ringID);
				assert(ejectionRing[e] != -1);
				assert(ejectorInUse[e]);
				assert(ejectionPID[e] == f->pid);
			}
			*/
			//end of checks

		}else if(ringStatus[input] == FORWARD){
			assert( ! cur_buf->Empty(0) );
			Flit * const f = cur_buf->FrontFlit(0);
			assert(f);
			int rID = f->ringID ;
			int output = _ringOutput[rID] ;
			assert(input == output);
			assert( ! f->head ) ;
			assert( ! ringReset[input] );
			input_to_output.push_back(make_pair(input, output));
			if(f->tail){
				ringReset[input] = true;
			}
		}else if(ringStatus[input] == IDLE) {

			if( ! cur_buf->Empty(0)){

				Flit * const f = cur_buf->FrontFlit(0);
				assert(f);
				assert(f->head);
				int rID = f->ringID ;
				int output = _ringOutput[rID] ;
				assert( input == output) ;
				assert( ! ringReset[input] );


				if(f->dest == id){
					//This packet didnt get the chance to eject. Wait for the next cycle
					//or forward if cur_buf is not full.
					if(cur_buf->Full()){
						ringStatus[input] = FORWARD ;
						input_to_output.push_back(make_pair(input, output));
						if(f->tail){
							ringReset[input] = true;
						}
					}else{
						ringStatus[input] = IDLE ;
						//wait for the next cycle.
					}
				}else{
					//This packet just arrived or moved to FIFO head and it shall be forward directly.
					//set ring to FORWARD state
					assert(ringStatus[input] == IDLE);
					ringStatus[input] = FORWARD ;
					input_to_output.push_back(make_pair(input, output));
					if(f->tail){
						ringReset[input] = true;
					}
				}
			}
		}else{
			//undefined ringStatus
			assert(false) ;
		}
	}
}



bool RLRouter::_canInjection(int input, int pSize/*packet Size */){
	if( ! useSpaceWhileStall) {
		return _buf[input]->Empty(0);
	}
	if(ringStatus[input] != IDLE){
		return false;
	}

	if(_buf[input]->Empty(0)) {
		assert(ringStatus[input] == IDLE);
		return true;
	}

	int available = _buf[input]->getSize() - _buf[input]->GetOccupancy();

	return available >= pSize ;

}

void RLRouter::_InjectionModule(){
	Buffer * const cur_buf = _buf[0]; // input from node
	if( cur_buf->Empty(0) ){
		return ; //nothing to do.
	}

	Flit * const f = _buf[0]->FrontFlit(0);
	assert(f);
	assert(f->src == GetID());
	if( ! f->head ){

		assert(injectionInUse && injectionRing != -1 && injectionOutput != -1);
		assert(ringStatus[injectionOutput] == INJECTION);
		assert( ! injectionReset && injectionPID == f->pid) ;
		f->ringID = injectionRing ;
		f->injectionTime = GetSimTime();

		input_to_output.push_back(make_pair(0, injectionOutput) );

		if(f->tail){
			injectionReset = true;
			ringReset[injectionOutput] = true ;
		}



	}else{

		assert( ! injectionInUse && injectionRing == -1 && injectionOutput == -1);
		assert( ! injectionReset && injectionPID == -1) ;

		for(int r = 0; r < _RL[f->dest].size(); r++){
			int rID = _RL[f->dest][r];
			int input = _ringInput[rID];

			if( _canInjection(input, f->pSize) ){

				injectionInUse = true ;
				injectionPID = f->pid;
				injectionRing = rID;
				injectionOutput = _ringOutput[rID];
				assert(injectionOutput == input);
				injectionReset = false ;

				f->ringID = rID ;
				f->injectionTime = GetSimTime();

				ringStatus[input] = INJECTION;

				input_to_output.push_back(make_pair(0, injectionOutput) );
				if(f->tail){
					injectionReset = true  ;
					ringReset[injectionOutput] = true ;

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


void RLRouter::_RouteTraffic(){
	//There is no need for any validity checks here, We had done enough above.
	//all traffic is routed here except for ejection traffic
	//We still remove all traffic from buffers here
/*	bool x[_outputs];
	for(int i = 0; i < _outputs; i++){
		x[i] = false ;
	}
	for(int i = 0; i < input_to_output.size(); i++){
		int input = input_to_output[i].first;
		int output = input_to_output[i].second;
		cout<<"input = "<<input<<", output = "<<output<<endl;
		if(x[output] && output != 0){
			cout<<"Output is used twice ! "<<endl;
			exit(0);
		}
		x[output] = true;
		Buffer * const cur_buf = _buf[input];
		cout<<GetSimTime()<<". Input = "<<input<<", output = "<<output<<endl;
		cout<<" Injection Ring = "<<injectionRing<<endl;
		cout<<" injectionOutput = "<<injectionOutput<<endl;
		cout<<" Buf Empty = "<<cur_buf->Empty(0) <<endl;
		cout<<"At "<<GetID()<<endl;
	}
	cout<<GetSimTime()<<"done"<<endl;
*/

 	for(int i = 0; i < input_to_output.size(); i++){

		int input = input_to_output[i].first;
		int output = input_to_output[i].second;

		Buffer * const cur_buf = _buf[input];

		assert( ! cur_buf->Empty(0) ) ;
		Flit * const f = cur_buf->FrontFlit(0);
		assert(f);

		if(input == 0){
			assert(output == injectionOutput);
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

void RLRouter::_OutputQueuing( )
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

void RLRouter::_SendFlits( )
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

void RLRouter::_SendCredits( )
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

void RLRouter::Display( ostream & os ) const
{
  for ( int input = 0; input < _inputs; ++input ) {
    _buf[input]->Display( os );
  }
}

int RLRouter::GetUsedCredit(int o) const
{
  assert((o >= 0) && (o < _outputs));
  BufferState const * const dest_buf = _next_buf[o];
  return dest_buf->Occupancy();
}

int RLRouter::GetBufferOccupancy(int i) const {
  assert(i >= 0 && i < _inputs);
  return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int RLRouter::GetUsedCreditForClass(int output, int cl) const
{
  assert((output >= 0) && (output < _outputs));
  BufferState const * const dest_buf = _next_buf[output];
  return dest_buf->OccupancyForClass(cl);
}

int RLRouter::GetBufferOccupancyForClass(int input, int cl) const
{
  assert((input >= 0) && (input < _inputs));
  return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> RLRouter::UsedCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->OccupancyFor(v);
    }
  }
  return result;
}

vector<int> RLRouter::FreeCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->AvailableFor(v);
    }
  }
  return result;
}

vector<int> RLRouter::MaxCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->LimitFor(v);
    }
  }
  return result;
}
