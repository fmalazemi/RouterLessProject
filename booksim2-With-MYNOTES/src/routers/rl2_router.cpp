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
THE EXTRA  ejector here is defined by doubling the ejection variables.
So if you want three you can add extra set of ejecation variables or
it might be the right time to have a parametric implementation.

*/




#include "rl2_router.hpp"

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

RL2Router::RL2Router( Configuration const & config, Module *parent,
		    string const & name, int id, int inputs, int outputs )
: Router( config, parent, name, id, inputs, outputs ), _active(false)
{




	ejectFlitPid = -1;
	ejectFlitInput = -1;
	ejectFlitRing = -1;
	eject = false ;
	ejectFlag = false ;


	ejectFlitPid_2 = -1;
	ejectFlitInput_2 = -1;
	ejectFlitRing_2 = -1;
	eject_2 = false ;
	ejectFlag = false ;

	inject = false ;
	injectFlitRing = -1 ;
	injectFlitPID = -1 ;
	injectFlitOutput = -1;

	_isRingBusy.resize(outputs);


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

RL2Router::~RL2Router( )
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

void RL2Router::AddInputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
  channel->setRLMode();
  backchannel->setRLMode();
	_ringInput.insert(make_pair(channel->GetRingID(), _input_channels.size()) );

	Router::AddInputChannel(channel, backchannel);
}



void RL2Router::AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel)
{
  channel->setRLMode();
  backchannel->setRLMode();
	_ringOutput.insert(make_pair(channel->GetRingID(), _output_channels.size()) );
  _next_buf[_output_channels.size()]->SetMinLatency(0);
  Router::AddOutputChannel(channel, backchannel);
}

void RL2Router::ReadInputs( )
{
  _ReceiveFlits( );
  _ReceiveCredits( );

}


void RL2Router::printBuffers(){
	cout<<GetSimTime()<<": At Router "<<GetID()<<endl;
for(int i = 0; i < _inputs; i++){
	cout<<"      buf["<<i<<"] = ";
		_buf[i]->Display2();
	}

		for(int i = 0; i < _outputs; i++){

			cout<<"      BufState["<<i<<"] = "<<_next_buf[i]->Occupancy()<<endl;

		}

}

void RL2Router::_InternalStep()
{
 	_InputQueuing( );
  _EjectionModule();
  _EjectionModule_2();
  _InputModule() ;
  _InjectionModule();
	_RouteTraffic();
	_ClearFlags();
  _OutputQueuing( );
  _bufferMonitor->cycle( );
  _switchMonitor->cycle( );

}

void RL2Router::WriteOutputs( )
{
  _SendFlits( );
  _SendCredits( );
}


//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool RL2Router::_ReceiveFlits( )
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
			if(f->pid == 15){
				cout<<GetSimTime()<<": PID = 15 is at "<<GetID()<<", input = "<<input<<", inputChannel(Receive Flit)"<<endl;
			}
      activity = true;
    }
  }
  return activity;
}

bool RL2Router::_ReceiveCredits( )
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

void RL2Router::_InputQueuing( )
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
		}
		//f->creationTime = GetSimTime();
		//f->injectionTime = GetSimTime();


    int const vc = f->vc;

		if(f->pid == 15){
			cout<<GetSimTime()<<": Flit 15 arrived at R"<<GetID()<<" ---- Input queuing"<<endl;
		}

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


void RL2Router::_ClearFlags(){
	//last call in every cycle

	if(eject == false){
		ejectFlitPid = ejectFlitRing = ejectFlitInput = -1;
		ejectFlag = false ;
	}
	if(eject_2 == false){
		ejectFlitPid_2 = ejectFlitRing_2 = ejectFlitInput_2 = -1;
		ejectFlag_2 = false ;
	}
	if(inject == false){
		injectFlitOutput = injectFlitPID = injectFlitRing = -1;
	}

}


void RL2Router::_EjectionModule(){
	if(eject){

		assert(ejectFlitPid != -1 && ejectFlitInput != -1 && ejectFlitRing != -1);

		//A previous Selection
		Flit * const f = _buf[ejectFlitInput]->FrontFlit(0);

		assert(f);
		assert(f->pid == ejectFlitPid);
		assert(f->ringID == ejectFlitRing);
		assert( ! f->head);
		assert(ejectFlag) ;
		f->arrivalTime = GetSimTime();

		//input_to_output.push_back(make_pair(ejectFlitInput, 0));

		if(f->tail){
			eject = false ;
		}
		return ;

	}
	assert(ejectFlitPid == -1 && ejectFlitInput == -1 && ejectFlitRing == -1);

	int maxRingID = -1;
	int maxPID = -1;
	int maxAge = -1 ;
	int maxInput = -1 ;
	int id = GetID();

	int curTime = GetSimTime();

	for(int input = 1; input < _inputs; input++){//ignore  core buf (_buf[0])

		if(_buf[input]->Empty(0) || ejectFlitInput_2 == input || injectFlitOutput == input) continue;

		Flit * const f = _buf[input]->FrontFlit(0);
		assert(f);
		if(f->dest == id && f->head){
			int age = curTime - f->creationTime;
			if(age > maxAge){
				maxAge = age;
				maxRingID = f->ringID;
				maxPID = f->pid ;
				maxInput = input;
			}
		}
	}
	if(maxPID != -1 && maxRingID != -1 && maxAge != -1 && maxInput != -1){
		Flit * const f = _buf[maxInput]->FrontFlit(0);

		assert(f->head && f->dest == id);
		f->arrivalTime = GetSimTime();



		ejectFlitPid = maxPID;
		ejectFlitRing = maxRingID;
		ejectFlitInput = maxInput;
		//input_to_output.push_back(make_pair(ejectFlitInput, 0));
		eject = true ;
		ejectFlag = true ;
		if(f->head && f->tail){
			eject = false ;
		}
		if(f->pid == 15)
			cout<<"Packet 15 set for ejection"<<endl;
	}


}

void RL2Router::_EjectionModule_2(){
	if(eject_2){

		assert(ejectFlitPid_2 != -1 && ejectFlitInput_2 != -1 && ejectFlitRing_2 != -1);

		//A previous Selection
		Flit * const f = _buf[ejectFlitInput_2]->FrontFlit(0);

		assert(f);
		assert(f->pid == ejectFlitPid_2);
		assert(f->ringID == ejectFlitRing_2);
		assert( ! f->head);
		assert(ejectFlag_2) ;
		f->arrivalTime = GetSimTime();

		//input_to_output.push_back(make_pair(ejectFlitInput, 0));

		if(f->tail){
			eject_2 = false ;
		}
		return ;

	}
	assert(ejectFlitPid_2 == -1 && ejectFlitInput_2 == -1 && ejectFlitRing_2 == -1);

	int maxRingID = -1;
	int maxPID = -1;
	int maxAge = -1 ;
	int maxInput = -1 ;
	int id = GetID();

	int curTime = GetSimTime();

	for(int input = 1; input < _inputs; input++){//ignore  core buf (_buf[0])

		if(_buf[input]->Empty(0) || ejectFlitInput == input || injectFlitOutput == input) continue;

		Flit * const f = _buf[input]->FrontFlit(0);
		assert(f);
		if(f->dest == id && f->head){
			int age = curTime - f->creationTime;
			if(age > maxAge){
				maxAge = age;
				maxRingID = f->ringID;
				maxPID = f->pid ;
				maxInput = input;
			}
		}
	}
	if(maxPID != -1 && maxRingID != -1 && maxAge != -1 && maxInput != -1){
		Flit * const f = _buf[maxInput]->FrontFlit(0);

		assert(f->head && f->dest == id);
		f->arrivalTime = GetSimTime();



		ejectFlitPid_2 = maxPID;
		ejectFlitRing_2 = maxRingID;
		ejectFlitInput_2 = maxInput;
		//input_to_output.push_back(make_pair(ejectFlitInput, 0));
		eject_2 = true ;

		ejectFlag_2 = true ;

		if(f->head && f->tail){
			eject_2 = false ;
		}

		if(f->pid == 15)
			cout<<"Packet 15 set for ejection"<<endl;
	}


}


void RL2Router::_InputModule(){
	//ejectionModule must execute before this function
	for(int input = 1; input < _inputs; input++){
		if(_buf[input]->Empty(0)) continue ;

		Flit * const f = _buf[input]->FrontFlit(0);
		assert(f);

		int rID = f->ringID;
		int output = _ringOutput[rID] ;


		//ERROR if flit recieved on wrong input
		assert( input == _ringInput[rID] );

		if( ! f->head){
			assert(_ringInput[rID] == _ringOutput[rID]); //Equivant to (input == output)
			if(f->pid == ejectFlitPid || f->pid == ejectFlitPid_2){
				assert(ejectFlag_2 || ejectFlag );
				assert(ejectFlitPid != ejectFlitPid_2);
				input_to_output.push_back(make_pair(input, 0) );
			} else {
				assert(_isRingBusy[output]);
				input_to_output.push_back(make_pair(input, output) );
			}
		}else{//process Head Flit
			if(f->dest == GetID()){
				if(f->pid == ejectFlitPid || f->pid == ejectFlitPid_2){
					assert(ejectFlag_2 || ejectFlag) ;
					assert(ejectFlitInput == input || ejectFlitInput_2 == input) ;
					assert(ejectFlitInput != ejectFlitInput_2);
					input_to_output.push_back(make_pair(input, 0));
				}else if(_buf[input]->Full()){
					//Another ring is using ouput 0 for ejection for buf is full
					assert( ! _isRingBusy[output] ) ;
					_isRingBusy[output] = true ;
					input_to_output.push_back(make_pair(input, output) );
				}else{
					//Another ring is using ouput 0 for ejection. The _buf[input] is not full yet. Block the flit!!
					//cout<<GetSimTime()<<". Flit (pid = "<<f->pid<<", id = "<<f->id<<") is blocked at input "<<input<<"."<<endl;
				}
			}else{ //the flit destiantion is not current router
				if( ! _isRingBusy[output] ){
					_isRingBusy[output] = true ;
					input_to_output.push_back(make_pair(input, output) );
				}else{
 					assert(injectFlitOutput == output);
					//The injection is using this ring to send traffic
					//cout<<GetSimTime()<<". Flit (pid = "<<f->pid<<", id = "<<f->id<<") is blocked at input "<<input<<"."<<endl;
				}
			}
		}
	}

}

void RL2Router::_InjectionModule(){

	if(_buf[0]->Empty(0)) return ;

 	Flit * const f = _buf[0]->FrontFlit(0);
	assert(f);

	if( ! f->head ) {
		assert(f->pid == injectFlitPID);
		assert(inject && injectFlitPID != -1 && injectFlitRing != -1 && injectFlitOutput != -1);

		f->ringID = injectFlitRing ;
		f->injectionTime = GetSimTime();


		//Shall update injection time for every flit here or just the head
		input_to_output.push_back(make_pair(0, injectFlitOutput) );
		if( f->tail ){
			inject = false ;
		}
		return ;
	}
	assert(f->head);

	for(int r = 0; r < _RL[f->dest].size(); r++){
		int rID = _RL[f->dest][r];
		int inputR = _ringInput[rID];

		if( _buf[inputR]->Empty(0) ){

			inject = true ;
			injectFlitPID = f->pid;
			injectFlitRing = rID;
			injectFlitOutput = _ringOutput[rID];

			f->ringID = rID ;
			f->injectionTime = GetSimTime();

			_isRingBusy[injectFlitOutput] = true;

			input_to_output.push_back(make_pair(0, injectFlitOutput) );
//			cout<<"Accepting Packet from R "<<GetID()<<endl;
			if(f->tail){
				inject = false ;
			}
			return ;//Found an empty ring buffer, we are done here! Exit function
		}

	}
//	cout<<"Packet "<<f->pid<<" is blocked"<<endl;


}



//------------------------------------------------------------------------------
// routing
//------------------------------------------------------------------------------

void RL2Router::_RouteTraffic(){
 	for(int i = 0; i < input_to_output.size(); i++){

		int input = input_to_output[i].first;
		int output = input_to_output[i].second;


		Buffer * const cur_buf = _buf[input];

		Flit * const f = cur_buf->FrontFlit(0);
		assert(f);

		if(input != 0){
			assert(_ringInput[f->ringID] == input);
		}
		if(output != 0){
			assert(_ringOutput[f->ringID] == output);
		}
		if(output == 0){
			assert(ejectFlag  || ejectFlag_2);
		}
		else {
			assert(_isRingBusy[output] || output == 0) ;//TODO: remove the || output=0
		}

		_output_buffer[output].push(f);


		cur_buf->RemoveFlit(0);


		_bufferMonitor->read(i, f) ;

		f->hops++;
		BufferState * const dest_buf = _next_buf[output];

		if(f->head){
		//	cout<<"HERE "<<endl;
		//	cout<<"input = "<<input<<", output = "<<output<<endl;
			dest_buf->TakeBuffer(0, input*1 + 0);
		}
		if(f->tail){
			_isRingBusy[output] = false ;
		}

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

		//_SendCredits();
		_switchMonitor->traversal(i, output, f) ;

		}
		input_to_output.clear();

}

//------------------------------------------------------------------------------
// output queuing
//------------------------------------------------------------------------------

void RL2Router::_OutputQueuing( )
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

void RL2Router::_SendFlits( )
{


  for ( int output = 0; output < _outputs; ++output ) {


    if ( !_output_buffer[output].empty( ) ) {

      Flit * const f = _output_buffer[output].front( );
      assert(f);


			if(f->pid == 15){
				cout<<GetSimTime()<<": PID = 15 is at "<<GetID()<<", output channel = "<<output<<" ------ SendFlits"<<endl;

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

void RL2Router::_SendCredits( )
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

void RL2Router::Display( ostream & os ) const
{
  for ( int input = 0; input < _inputs; ++input ) {
    _buf[input]->Display( os );
  }
}

int RL2Router::GetUsedCredit(int o) const
{
  assert((o >= 0) && (o < _outputs));
  BufferState const * const dest_buf = _next_buf[o];
  return dest_buf->Occupancy();
}

int RL2Router::GetBufferOccupancy(int i) const {
  assert(i >= 0 && i < _inputs);
  return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int RL2Router::GetUsedCreditForClass(int output, int cl) const
{
  assert((output >= 0) && (output < _outputs));
  BufferState const * const dest_buf = _next_buf[output];
  return dest_buf->OccupancyForClass(cl);
}

int RL2Router::GetBufferOccupancyForClass(int input, int cl) const
{
  assert((input >= 0) && (input < _inputs));
  return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> RL2Router::UsedCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->OccupancyFor(v);
    }
  }
  return result;
}

vector<int> RL2Router::FreeCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->AvailableFor(v);
    }
  }
  return result;
}

vector<int> RL2Router::MaxCredits() const
{
  vector<int> result(_outputs*_vcs);
  for(int o = 0; o < _outputs; ++o) {
    for(int v = 0; v < _vcs; ++v) {
      result[o*_vcs+v] = _next_buf[o]->LimitFor(v);
    }
  }
  return result;
}

void RL2Router::_UpdateNOQ(int input, int vc, Flit const * f) {


}
