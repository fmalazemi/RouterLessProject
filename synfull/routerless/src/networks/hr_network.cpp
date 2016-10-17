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


#include "hr_network.hpp"
#include <fstream>
#include <sstream>
#include <limits>
#include <algorithm>

HRNetwork::HRNetwork( const Configuration &config, const string & name )
  :  Network( config, name ){

  _ComputeSize( config );
  _Alloc();
  _BuildNet(config);

}

HRNetwork::~HRNetwork(){
  for(int i = 0; i < Ring.size(); i++){
    Ring[i].clear();
  }
  Ring.clear();

}

void HRNetwork::_ComputeSize( const Configuration &config ){
  file_name = config.GetStr("network_file");
  if(file_name==""){
    cout<<"No network file name provided"<<endl;
    exit(-1);
  }
  readFile();

}

void HRNetwork::readRing(ifstream & in /*input file*/){

  
  in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

  string line ;
  getline(in,line);
  int n = atoi(line.c_str()); //number of Rings
  cout<<"Number of rings = "<<n<<", line = "<<line<<endl;
  Ring.resize(n);
  for(int i = 0; i < n; i++){
    getline(in,line);
    replace(line.begin(), line.end(), ':', ' ');
    replace(line.begin(), line.end(), '[', ' ');
    replace(line.begin(), line.end(), ']', ' ');
    replace(line.begin(), line.end(), ',', ' ');
    stringstream str;
    str<<line;

    int id, node_id;
    str>>id ; //local ring id

    while(str>>node_id){
      Ring[id].push_back(node_id);
    }
  }
}

int HRNetwork::countChannels(){
  int count = 0;
  for(int i = 0; i < Ring.size(); i++){
    count += Ring[i].size();

  }
  cout<<"The number of channels = "<<count<<endl;

  return count;
}


void HRNetwork::readFile(){

  ifstream in;//input file
  string line;



  in.open(file_name.c_str());
  if(!in.is_open()){
    cout<<"HR_Network:can't open network file "<<file_name<<endl;
    exit(-1);
  }
  in >> _nLr;  // number of local routers   
  in >> _nGr;  // number of global routers 

  readRing(in);

  _size = _nLr + _nGr; //number of local and global routers
  _nodes = _nLr ; // number of cores
  _channels = countChannels();


}


void HRNetwork::_BuildNet(const Configuration &config){

  cout<<"=========================================================="<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"============  {---- Ring Configuration ----} ============="<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"==========================================================\n\n\n"<<endl;

  cout<<"------------------ [ Router-to-Node ] -------------------"<<endl;

  for(int node = 0; node < _nLr; node++){
    //for Local routers, there will be 3 input and 3 output (one from core and 2 for local ring)
    int radix = 3 ;  
    //every ring need two channels to pass through a node.

    ostringstream router_name;
    router_name << "router";
    router_name << "_" <<  node ;



    _routers[node] = Router::NewRouter( config, this, router_name.str(), node, radix, radix);

    _timed_modules.push_back(_routers[node]);


    _inject[node]->SetLatency(1);
    _inject_cred[node]->SetLatency(1);

    _inject[node]->SetRingID(-1);
    _inject_cred[node]->SetRingID(-1);

    _routers[node]->AddInputChannel(_inject[node], _inject_cred[node]);


    _eject[node]->SetLatency(1);
    _eject_cred[node]->SetLatency(1);

    _eject[node]->SetRingID(-1);
    _eject_cred[node]->SetRingID(-1);

    _routers[node]->AddOutputChannel(_eject[node], _eject_cred[node]);

    cout<<_routers[node]->GetID()<<" -to- "<<node<<endl;


  }
  //
  for(int r = _nLr; r < _nLr+_nGr; r++){
    int radix = 4; //radix
    ostringstream router_name;
    router_name << "router";
    router_name << "_" <<  r ;
    _routers[r] = Router::NewRouter( config, this, router_name.str(), r, radix, radix);
    _timed_modules.push_back(_routers[r]);
    cout<<"Global Router "<<_routers[r]->GetID()<<" is Created "<<r<<endl;


  
  }

  int curChannel = 0;
  cout<<"\n\n------------------- [ Rings List ] --------------------\n\n"<<endl;
  cout<<"Numebr of rings = "<<Ring.size()<<endl;
  for(int rID = 0; rID < Ring.size(); rID++){

      assert(Ring[rID].size() > 1);
      cout<<"Ring ID = "<<rID <<endl;
      cout<<"Ring List = [";

      for(int i = 0; i < Ring[rID].size()-1; i++){

        int node = Ring[rID][i];
        int nextNode = Ring[rID][i+1];

        _chan[curChannel]->SetLatency(1);
        _chan_cred[curChannel]->SetLatency(1);


        _chan[curChannel]->SetRingID(rID);
        _chan_cred[curChannel]->SetRingID(rID);

        _routers[node]->AddOutputChannel( _chan[curChannel], _chan_cred[curChannel] );
        _routers[nextNode]->AddInputChannel( _chan[curChannel], _chan_cred[curChannel]);

        curChannel++;
        cout<<_routers[node]->GetID()<<", ";

      }

      int node = Ring[rID][Ring[rID].size()-1];
      cout<<_routers[node]->GetID()<<" ]"<<endl;

      int nextNode = Ring[rID][0];
      _chan[curChannel]->SetLatency(1);
      _chan_cred[curChannel]->SetLatency(1);

      _chan[curChannel]->SetRingID(rID);
      _chan_cred[curChannel]->SetRingID(rID);

      _routers[node]->AddOutputChannel( _chan[curChannel], _chan_cred[curChannel] );
      _routers[nextNode]->AddInputChannel( _chan[curChannel], _chan_cred[curChannel]);
      curChannel++;

  }

  for(int i = 0; i < _routers.size(); i++){
    cout<<"At router "<<_routers[i]->GetID()<<" = "<<i<<endl;
    int j = 0; 
    if(i < _nLr){
         j = 1; 
    } 
    for( ; j < _routers[i]->NumInputs(); j++){
        FlitChannel * x = _routers[i]->GetInputChannel(j);
        Router const * source = x->GetSource();
        Router const * dest = x->GetSink();
        cout<<"input   "<<j<<" is a channel to "<<dest->GetID()<<" from "<<source->GetID()<<" (Ring "<<x->GetRingID()<<")"<<endl;
    }
    j = 0;
    if(i < _nLr){ 
         j = 1;
    }
    for( ; j < _routers[i]->NumOutputs(); j++){
        FlitChannel * x = _routers[i]->GetOutputChannel(j);
        Router const * source = x->GetSource();
        Router const * dest = x->GetSink();
        cout<<"Output  "<<j<<" is a channel from "<<source->GetID()<<" to "<<dest->GetID()<<" (Ring "<<x->GetRingID()<<")"<<endl;

    }
  }




  cout<<"\n\n====================  {---- END ----} ====================\n\n"<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"============  {---- Starting Simulation ----} ============"<<endl;
  cout<<"=========================================================="<<endl;
  cout<<"==========================================================\n\n\n"<<endl;



}





void HRNetwork::RegisterRoutingFunctions() {
  gRoutingFunctionMap["min_hr"] = &min_hr;

}

void min_hr( const Router *r, const Flit *f, int in_channel,
		 OutputSet *outputs, bool inject ){

  int out_port=-1;
  if(!inject){
    out_port = 0;
    if(f->dest != r->GetID()){
      out_port = (f->id%2)+1;
    }
    out_port = 0;
  }


  int vcBegin = 0, vcEnd = gNumVCs-1;
  if ( f->type == Flit::READ_REQUEST ) {
    vcBegin = gReadReqBeginVC;
    vcEnd   = gReadReqEndVC;
  } else if ( f->type == Flit::WRITE_REQUEST ) {
    vcBegin = gWriteReqBeginVC;
    vcEnd   = gWriteReqEndVC;
  } else if ( f->type ==  Flit::READ_REPLY ) {
    vcBegin = gReadReplyBeginVC;
    vcEnd   = gReadReplyEndVC;
  } else if ( f->type ==  Flit::WRITE_REPLY ) {
    vcBegin = gWriteReplyBeginVC;
    vcEnd   = gWriteReplyEndVC;
  }

  outputs->Clear( );

  outputs->AddRange( out_port , vcBegin, vcEnd );
}
