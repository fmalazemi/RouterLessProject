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




#ifndef _RLALPHA_ROUTER_HPP_
#define _RLALPHA_ROUTER_HPP_

#include <string>
#include <deque>
#include <queue>
#include <set>
#include <map>

#include "router.hpp"
#include "routefunc.hpp"

using namespace std;

class VC;
class Flit;
class Credit;
class Buffer;
class BufferState;
class Allocator;
class SwitchMonitor;
class BufferMonitor;

class RLALPHARouter : public Router {

  int _vcs;


  struct ej{
    bool inUse;
    int ringID;
    int input;
    bool reset;
    int pid;
  };

  vector<ej> ejector;

  int _ejectors;


  



  // we usually have 1 injector, but we may have more. If we do, change the
  //the below variables to vectors

  struct inj {
    int ringID ;
    int pid ;
    int output ;
    bool inUse ;
    bool reset ;
  };
  //vector<inj> injector; ///use a vector for multiple injectors
  inj injector;


  int _injectors;
  // bool injectorInUse ; //the size is _injectors


  struct rState{//ring state
        bool isEjecting;
        int  ejectorID;
        bool isInjecting;
        bool isForward ; //forward is reset with last flit
        bool resetForward;
  };
  vector<rState> ringState;




  bool useSpaceWhileStall;





  static const int infinite = 2147483647;
  vector<pair<int, int> > input_to_output;
  map<int, int> _ringOutput; // a mapping from outputPort to RingID
  map<int, int> _ringInput;




  int injectFlitPID;
  int injectFlitRing;
  int injectFlitOutput;
  bool inject ;




  bool _active;

  map<int, Flit *> _in_queue_flits;




  deque<pair<int, pair<Credit *, int> > > _proc_credits;


  map<int, Credit *> _out_queue_credits;

  vector<Buffer *> _buf;
  vector<BufferState *> _next_buf;

  tRoutingFunction   _rf;

  int _output_buffer_size;
  vector<queue<Flit *> > _output_buffer;

  vector<queue<Credit *> > _credit_buffer;



  bool _ReceiveFlits( );
  bool _ReceiveCredits( );
  void printBuffers();
  virtual void _InternalStep( );

  bool _SWAllocAddReq(int input, int vc, int output);

  void _InputQueuing( );

  void _ClearFlags();
  void _InjectionModule();
  void _InputModule();
  void _EjectionModule() ;
  void _RouteTraffic();
  int _oldestPacket() ;
  bool _canInjection(int, int);
  int _getEjectorID(int );
  void _resetStatus();
  void _initialReset();





  void _OutputQueuing( );

  void _SendFlits( );
  void _SendCredits( );

  void _UpdateNOQ(int input, int vc, Flit const * f);

  // ----------------------------------------
  //
  //   Router Power Modellingyes
  //
  // ----------------------------------------

  SwitchMonitor * _switchMonitor ;
  BufferMonitor * _bufferMonitor ;

public:

  RLALPHARouter( Configuration const & config,
	    Module *parent, string const & name, int id,
	    int inputs, int outputs );

  virtual ~RLALPHARouter( );

  virtual void AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel);
  virtual void AddInputChannel(FlitChannel * channel, CreditChannel * backchannel);

  virtual void ReadInputs( );
  virtual void WriteOutputs( );

  void Display( ostream & os = cout ) const;

  virtual int GetUsedCredit(int o) const;
  virtual int GetBufferOccupancy(int i) const;

#ifdef TRACK_BUFFERS
  virtual int GetUsedCreditForClass(int output, int cl) const;
  virtual int GetBufferOccupancyForClass(int input, int cl) const;
#endif

  virtual vector<int> UsedCredits() const;
  virtual vector<int> FreeCredits() const;
  virtual vector<int> MaxCredits() const;

  SwitchMonitor const * const GetSwitchMonitor() const {return _switchMonitor;}
  BufferMonitor const * const GetBufferMonitor() const {return _bufferMonitor;}

};

#endif
