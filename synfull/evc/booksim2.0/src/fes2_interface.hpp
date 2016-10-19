/*
 * fes2_interface.hpp
 *
 *  Created on: Apr 10, 2010
 *      Author: robert
 */

#ifndef FES2_INTERFACE_HPP_
#define FES2_INTERFACE_HPP_

//#define FES2_INTERFACE_ACTIVE

#include "module.hpp"
#include "config_utils.hpp"
#include "network.hpp"
#include "socketstream.h"
#include "messages.h"
#include "trace_generator.hpp"

#include <queue>
#include <map>


struct FeS2RequestPacket {
	int source;
	int dest;
	int id;
	int size;
	int network;
	int cl;
	int miss_pred;
};

struct FeS2ReplyPacket {
	int source;
	int dest;
	int id;
	int network;
	int cl;
	int miss_pred;
};


class FeS2Interface {

private:

	int numOfSyn;

	vector<queue<FeS2RequestPacket *> **> _request_buffer;
	vector<queue<FeS2ReplyPacket *> > _reply_buffer;

	vector<map<int, int> > _original_destinations;

	TraceGenerator *_trace;

	int _trace_mode;

	int _sources;
	int _dests;

	vector<int> _duplicate_networks;

	vector<bool> _concentrate;

	vector<SocketStream> _listenSocket;
	vector<SocketStream *>_channel;
	string _host;
	int _port;

	vector<map<int, int> > _node_map;
	map<int, int> _synfull_map;
	map<int, bool> synfullDone; 

	FeS2RequestPacket *DequeueFeS2RequestPacket(int source, int network, int cl, int sid);
        int EnqueueFeS2ReplyPacket(FeS2ReplyPacket *packet, int sid);
	int GetSynfullID(int s); //send source or dest and get Synfull ID ;

public:
	FeS2Interface( const Configuration &config, const vector<Network *> & net );
	~FeS2Interface();

	int Init();
	int Init(int);
	int Step();
	int Step(int); 

	int EnqueueFeS2RequestPacket(FeS2RequestPacket *packet, int sid);
	FeS2RequestPacket *DequeueFeS2RequestPacket(int source, int network, int cl);

	int getFeS2ReplyQueueSize(int sid) { return _reply_buffer[sid].size(); }
	int EnqueueFeS2ReplyPacket(FeS2ReplyPacket *packet);
	FeS2ReplyPacket *DequeueFeS2ReplyPacket(int sid);

	int GenerateTestPackets();


};

#endif /* FES2_INTERFACE_HPP_ */
