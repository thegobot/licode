/*
 * NiceConnection.cpp
 */

#include <glib.h>
#include <nice/nice.h>
#include <cstdio>

#include "NiceConnection.h"
#include "SdpInfo.h"


// If true (and configured properly below) erizo will generate relay candidates for itself MOSTLY USEFUL WHEN ERIZO ITSELF IS BEHIND A NAT
#define SERVER_SIDE_TURN 0

namespace erizo {
  
  DEFINE_LOGGER(NiceConnection, "NiceConnection");
  guint stream_id;
  GSList* lcands;
  int streamsGathered;
  int rec, sen;
  int length;

  void cb_nice_recv(NiceAgent* agent, guint stream_id, guint component_id,
      guint len, gchar* buf, gpointer user_data) {

    NiceConnection* nicecon = (NiceConnection*) user_data;
    nicecon->getNiceListener()->onNiceData(component_id, reinterpret_cast<char*> (buf), static_cast<unsigned int> (len),
        (NiceConnection*) user_data);
  }

  void cb_new_candidate(NiceAgent *agent, guint stream_id, guint component_id, gchar *foundation,
      gpointer user_data) {

    NiceConnection *conn = (NiceConnection*) user_data;
    std::string found(foundation);
    conn->getCandidate(stream_id, component_id, found);
  }

  void cb_candidate_gathering_done(NiceAgent *agent, guint stream_id,
      gpointer user_data) {

    NiceConnection *conn = (NiceConnection*) user_data;
    //conn->gatheringDone(stream_id);
  }

  void cb_component_state_changed(NiceAgent *agent, guint stream_id,
      guint component_id, guint state, gpointer user_data) {
    if (state == NICE_COMPONENT_STATE_READY) {
      NiceConnection *conn = (NiceConnection*) user_data;
      conn->updateComponentState(component_id, NICE_READY);
    } else if (state == NICE_COMPONENT_STATE_FAILED) {
      NiceConnection *conn = (NiceConnection*) user_data;
      conn->updateIceState(NICE_FAILED);
    }

  }

  void cb_new_selected_pair(NiceAgent *agent, guint stream_id, guint component_id,
      gchar *lfoundation, gchar *rfoundation, gpointer user_data) {
    NiceConnection *conn = (NiceConnection*) user_data;
    conn->updateComponentState(component_id, NICE_READY);
  }

  NiceConnection::NiceConnection(MediaType med,
      const std::string &transport_name, unsigned int iceComponents, const std::string& stunServer,
      int stunPort, int minPort, int maxPort):mediaType(med), iceComponents_(iceComponents),
  stunServer_(stunServer), stunPort_ (stunPort), minPort_(minPort), maxPort_(maxPort) {
    agent_ = NULL;
    loop_ = NULL;
    listener_ = NULL;
    localCandidates.reset(new std::vector<CandidateInfo>());
    transportName.reset(new std::string(transport_name));
    for (unsigned int i = 1; i<=iceComponents; i++) {
      comp_state_list[i] = NICE_INITIAL;
    }

  }

  NiceConnection::~NiceConnection() {
    ELOG_DEBUG("NiceConnection Destructor");
    if (iceState != NICE_FINISHED){
      if (loop_ != NULL){
        if (g_main_loop_is_running(loop_)){
          g_main_loop_quit(loop_);
        }
      }
      iceState = NICE_FINISHED;
    }
    m_Thread_.join();
    if (agent_!=NULL){
      g_object_unref(agent_);
      agent_ = NULL;
    }
    if (loop_ != NULL) {
      g_main_loop_unref (loop_);
      loop_=NULL;
    }
  }

  void NiceConnection::start() {

    m_Thread_ = boost::thread(&NiceConnection::init, this);
  }

  int NiceConnection::sendData(unsigned int compId, const void* buf, int len) {
    boost::mutex::scoped_lock lock(writeMutex_);
    int val = -1;
    if (iceState == NICE_READY) {
      val = nice_agent_send(agent_, 1, compId, len, reinterpret_cast<const gchar*>(buf));
    }
    if (val != len) {
      ELOG_DEBUG("Data sent %d of %d", val, len);
    }
    return val;
  }

  void NiceConnection::init() {

    streamsGathered = 0;
    

    g_type_init();
    ELOG_DEBUG("Creating Main Context");
    context_ = g_main_context_new();
    ELOG_DEBUG("Creating Main Loop");
    loop_ =  g_main_loop_new(context_, FALSE);
    ELOG_DEBUG("Creating Agent");
    //loop_ =  g_main_loop_new(NULL, FALSE);
    // nice_debug_enable( TRUE );
    // Create a nice agent
    //agent_ = nice_agent_new(g_main_loop_get_context(loop_), NICE_COMPATIBILITY_RFC5245);
    agent_ = nice_agent_new(context_, NICE_COMPATIBILITY_RFC5245);
    GValue controllingMode = { 0 };
    g_value_init(&controllingMode, G_TYPE_BOOLEAN);
    g_value_set_boolean(&controllingMode, true);
    g_object_set_property(G_OBJECT( agent_ ), "controlling-mode", &controllingMode);

    GValue checks = { 0 };
    g_value_init(&checks, G_TYPE_UINT);
    g_value_set_uint(&checks, 100);
    g_object_set_property(G_OBJECT( agent_ ), "max-connectivity-checks", &checks);

    //	NiceAddress* naddr = nice_address_new();
    //	nice_agent_add_local_address(agent_, naddr);

    if (stunServer_.compare("") != 0 && stunPort_!=0){
      GValue val = { 0 }, val2 = { 0 };
      g_value_init(&val, G_TYPE_STRING);
      g_value_set_string(&val, stunServer_.c_str());
      g_object_set_property(G_OBJECT( agent_ ), "stun-server", &val);

      g_value_init(&val2, G_TYPE_UINT);
      g_value_set_uint(&val2, stunPort_);
      g_object_set_property(G_OBJECT( agent_ ), "stun-server-port", &val2);

      ELOG_DEBUG("Setting STUN server %s:%d", stunServer_.c_str(), stunPort_);
    }

    // Connect the signals
    g_signal_connect( G_OBJECT( agent_ ), "candidate-gathering-done",
        G_CALLBACK( cb_candidate_gathering_done ), this);
    g_signal_connect( G_OBJECT( agent_ ), "new-selected-pair",
        G_CALLBACK( cb_new_selected_pair ), this);
    g_signal_connect( G_OBJECT( agent_ ), "component-state-changed",
        G_CALLBACK( cb_component_state_changed ), this);
    g_signal_connect( G_OBJECT( agent_ ), "new-candidate",
        G_CALLBACK( cb_new_candidate ), this);

    // Create a new stream and start gathering candidates
    ELOG_DEBUG("Adding Stream... Number of components %d", iceComponents_);
    nice_agent_add_stream(agent_, iceComponents_);

    gchar *ufrag, *upass;
    nice_agent_get_local_credentials(agent_, 1, &ufrag, &upass);
    ufrag_ = std::string(ufrag);
    upass_ = std::string(upass);

    this->updateIceState(NICE_INITIAL);
    
    // Set Port Range ----> If this doesn't work when linking the file libnice.sym has to be modified to include this call
   
    if (minPort_!=0 && maxPort_!=0){
      ELOG_DEBUG("Setting port range: %d to %d\n", minPort_, maxPort_);
      nice_agent_set_port_range(agent_, (guint)1, (guint)1, (guint)minPort_, (guint)maxPort_);
    }
    
    if (SERVER_SIDE_TURN){
        for (int i = 1; i < (iceComponents_ +1); i++){
          ELOG_DEBUG("Setting TURN Comp %d", i);
          nice_agent_set_relay_info     (agent_,
              1,
              i,
              "",      // TURN Server IP
              3479,    // TURN Server PORT
              "",      // Username
              "",      // Pass
              NICE_RELAY_TYPE_TURN_UDP); 
        }
    }

    ELOG_DEBUG("Gathering candidates");
    nice_agent_gather_candidates(agent_, 1);
    nice_agent_attach_recv(agent_, 1, 1, context_,
        cb_nice_recv, this);
    if (iceComponents_ > 1) {
      nice_agent_attach_recv(agent_, 1, 2, context_,
          cb_nice_recv, this);
    }
    
    // Attach to the component to receive the data
    g_main_loop_run(loop_);

  }

  bool NiceConnection::setRemoteCandidates(
      std::vector<CandidateInfo> &candidates) {

    GSList* candList = NULL;
    int currentCompId = 1;
    for (unsigned int it = 0; it < candidates.size(); it++) {
      NiceCandidateType nice_cand_type;
      CandidateInfo cinfo = candidates[it];

      switch (cinfo.hostType) {
        case HOST:
          nice_cand_type = NICE_CANDIDATE_TYPE_HOST;
          break;
        case SRFLX:
          nice_cand_type = NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
          break;
        case PRFLX:
          nice_cand_type = NICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
          break;
        case RELAY:
          nice_cand_type = NICE_CANDIDATE_TYPE_RELAYED;
          break;
        default:
          nice_cand_type = NICE_CANDIDATE_TYPE_HOST;
          break;
      }
      NiceCandidate* thecandidate = nice_candidate_new(nice_cand_type);
      thecandidate->username = strdup(cinfo.username.c_str());
      thecandidate->password = strdup(cinfo.password.c_str());
      thecandidate->stream_id = (guint) 1;
      thecandidate->component_id = cinfo.componentId;
      thecandidate->priority = cinfo.priority;
      thecandidate->transport = NICE_CANDIDATE_TRANSPORT_UDP;
      nice_address_set_from_string(&thecandidate->addr, cinfo.hostAddress.c_str());
      nice_address_set_port(&thecandidate->addr, cinfo.hostPort);
      currentCompId = cinfo.componentId;
      
      if (cinfo.hostType == RELAY||cinfo.hostType==SRFLX){
        nice_address_set_from_string(&thecandidate->base_addr, cinfo.rAddress.c_str());
        nice_address_set_port(&thecandidate->base_addr, cinfo.rPort);
        ELOG_DEBUG("Adding remote candidate type %d addr %s port %d raddr %s rport %d", cinfo.hostType, cinfo.hostAddress.c_str(), cinfo.hostPort,
            cinfo.rAddress.c_str(), cinfo.rPort);
      }else{
        ELOG_DEBUG("Adding remote candidate type %d addr %s port %d priority %d componentId %d", 
          cinfo.hostType, 
          cinfo.hostAddress.c_str(), 
          cinfo.hostPort, 
          cinfo.priority, 
          cinfo.componentId
          );
      }
      candList = g_slist_prepend(candList, thecandidate);
    }
    nice_agent_set_remote_candidates(agent_, (guint) 1, currentCompId, candList);
    g_slist_free_full(candList, (GDestroyNotify)&nice_candidate_free);

    this->updateIceState(NICE_CANDIDATES_RECEIVED);
    return true;
  }

  void NiceConnection::getCandidate(uint stream_id, uint component_id, const std::string &foundation) {
    int currentCompId = 1;
    lcands = nice_agent_get_local_candidates(agent_, stream_id, component_id);
    NiceCandidate *cand;
    GSList* iterator;

    //  ELOG_DEBUG("gathering done %u",stream_id);
    for (iterator = lcands; iterator; iterator = iterator->next) {
      cand = (NiceCandidate*) iterator->data;
      if (cand->component_id == component_id && cand->foundation == foundation) {
        char address[40], baseAddress[40];
        nice_address_to_string(&cand->addr, address);
        nice_address_to_string(&cand->base_addr, baseAddress);
        if (strstr(address, ":") != NULL) {
          //ELOG_DEBUG("Ignoring IPV6 candidate %s", address);
          continue;
        }
        CandidateInfo cand_info;
        cand_info.componentId = cand->component_id;
        cand_info.foundation = cand->foundation;
        cand_info.priority = cand->priority;
        cand_info.hostAddress = std::string(address);
        cand_info.hostPort = nice_address_get_port(&cand->addr);
        cand_info.mediaType = mediaType;

        /*
         *   NICE_CANDIDATE_TYPE_HOST,
         *    NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE,
         *    NICE_CANDIDATE_TYPE_PEER_REFLEXIVE,
         *    NICE_CANDIDATE_TYPE_RELAYED,
         */
        switch (cand->type) {
          case NICE_CANDIDATE_TYPE_HOST:
            cand_info.hostType = HOST;
            break;
          case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
            cand_info.hostType = SRFLX;
            cand_info.rAddress = std::string(baseAddress);
            cand_info.rPort = nice_address_get_port(&cand->base_addr);
            break;
          case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
            cand_info.hostType = PRFLX;
            break;
          case NICE_CANDIDATE_TYPE_RELAYED:
            char turnAddres[40];
            ELOG_DEBUG("TURN LOCAL CANDIDATE");
            nice_address_to_string(&cand->turn->server,turnAddres);
            ELOG_DEBUG("address %s", address);
            ELOG_DEBUG("baseAddress %s", baseAddress);
            ELOG_DEBUG("stream_id %u", cand->stream_id);
            ELOG_DEBUG("priority %u", cand->priority);
            ELOG_DEBUG("TURN ADDRESS %s", turnAddres);
           
            cand_info.hostType = RELAY;
            cand_info.rAddress = std::string(baseAddress);
            cand_info.rPort = nice_address_get_port(&cand->base_addr);

            break;
          default:
            break;
        }
        cand_info.netProtocol = "udp";
        cand_info.transProtocol = std::string(*transportName.get());

        cand_info.username = ufrag_;

        cand_info.password = upass_;
        /*
           if (cand->username)
           cand_info.username = std::string(cand->username);
           else
           cand_info.username = std::string("(null)");

           if (cand->password)
           cand_info.password = std::string(cand->password);
           else
           cand_info.password = std::string("(null)");
           */

        localCandidates->push_back(cand_info);
        this->getNiceListener()->onCandidate(cand_info, this);
      }
    }
    ELOG_INFO("New local candidate");

    updateIceState(NICE_CANDIDATES_GATHERED);
  }

  void NiceConnection::getLocalCredentials(std::string *username, std::string *password) {
    *username = std::string(ufrag_);
    *password = std::string(upass_);
  }

  void NiceConnection::gatheringDone(uint stream_id) {
   
  }

  void NiceConnection::setNiceListener(NiceConnectionListener *listener) {
    this->listener_ = listener;
  }

  NiceConnectionListener* NiceConnection::getNiceListener() {
    return this->listener_;
  }

  void NiceConnection::updateComponentState(unsigned int compId, IceState state) {
    ELOG_DEBUG("%s - NICE Component State Changed %u - %u", transportName->c_str(), compId, state);
    comp_state_list[compId] = state;
    if (state == NICE_READY) {
      for (unsigned int i = 1; i<=iceComponents_; i++) {
        if (comp_state_list[i] != NICE_READY) {
          return;
        }
      }
    }
    this->updateIceState(state);
  }

  void NiceConnection::updateIceState(IceState state) {
    ELOG_DEBUG("%s - NICE State Changed %u", transportName->c_str(), state);
    this->iceState = state;
    if (state == NICE_READY){
      char ipaddr[30];
      NiceCandidate* local, *remote;
      nice_agent_get_selected_pair(agent_, 1, 1, &local, &remote); 
      nice_address_to_string(&local->addr, ipaddr);
      ELOG_DEBUG("Selected pair:\nlocal candidate addr: %s:%d",ipaddr, nice_address_get_port(&local->addr));
      nice_address_to_string(&remote->addr, ipaddr);
      ELOG_DEBUG("remote candidate addr: %s:%d",ipaddr, nice_address_get_port(&remote->addr));
    }
    if (this->listener_ != NULL)
      this->listener_->updateIceState(state, this);
  }

} /* namespace erizo */
