//////////////////////////////////////////////////////////////////////////////
// Copyright 2017-2020 Lawrence Livermore National Security, LLC and other
// UMAP Project Developers. See the top-level LICENSE file for details.
//
// SPDX-License-Identifier: LGPL-2.1-only
//////////////////////////////////////////////////////////////////////////////
#include <cassert>
#include <unistd.h>
#include <mpi.h>

#include "umap/util/Macros.hpp"
#include "rpc_server.hpp"
#include "rpc_util.hpp"

static const char* PROTOCOL_MARGO_SHM   = "na+sm://";
static const char* PROTOCOL_MARGO_VERBS = "ofi+verbs://";
static const char* PROTOCOL_MARGO_TCP   = "bmi+tcp://";
static const char* PROTOCOL_MARGO_MPI   = "mpi+static";

static std::map<const char*, RemoteMemoryObject> remote_memory_pool;
static int server_id=-1;
static int num_completed_clients=0;
static int num_clients=0;

void print_server_memory_pool()
{
  for(auto it : remote_memory_pool)
    UMAP_LOG(Info, "Server "<< server_id
	     <<"remote_memory_pool[ " << it.first << " ] :: "
	     <<(it.second).ptr << ", " <<(it.second).rsize);
}

int server_add_resource(const char*id, void* ptr, size_t rsize){
  int ret = 0;

  /* Register the remote memory object to the pool */
  if( remote_memory_pool.find(id)!=remote_memory_pool.end() ){
    UMAP_ERROR("Cannot create datastore with duplicated name: "<< id);
    return -1;
  }
  remote_memory_pool.emplace(id, RemoteMemoryObject(ptr, rsize ));
  
  print_server_memory_pool();
  return ret;
}

int server_delete_resource(const char* id){
  int ret = 0;
  
  assert(remote_memory_pool.find(id)!=remote_memory_pool.end());
  remote_memory_pool.erase(id);
  print_server_memory_pool();
  
  if(remote_memory_pool.size()==0){
    UMAP_LOG(Info, "shuting down Server " << server_id);
    server_fini();
  }
  
  return ret;
}

void publish_server_addr(const char* addr)
{
    /* write server address to local file for client to read */
    FILE* fp = fopen(LOCAL_RPC_ADDR_FILE, "w+");
    if (fp != NULL) {
        fprintf(fp, "%s", addr);
        fclose(fp);
    } else {
      UMAP_ERROR("Error writing server rpc addr file "<<LOCAL_RPC_ADDR_FILE);
    }
}

char* get_memory_object(const char* id, size_t offset, size_t size)
{
  char* ptr = NULL;
  std::map<const char*, RemoteMemoryObject>::iterator it = remote_memory_pool.find(id);
  if( it==remote_memory_pool.end() ){
    /*TODO */
    UMAP_ERROR("Request "<<id<<" not found");
    return ptr;
  }
  
  RemoteMemoryObject obj = it->second;
  assert( obj.ptr!=NULL);
  assert( (offset+size) <= obj.rsize );
  ptr = (char*)obj.ptr;
  
  return ptr;
}
  
/* 
 * The read rpc is executed on the server 
 * when the client request arrives
 * it starts bulk transfer to the client
 * when it returns, it callls client's rpc complete 
 * callback function if defined in HG_Foward()
 */
static int umap_server_read_rpc(hg_handle_t handle)
{
  UMAP_LOG(Debug, "Entering");

  assert(mid != MARGO_INSTANCE_NULL);
  
  hg_return_t ret;

  /* get Mercury info */
  /* margo instance id is similar to mercury context */
  const struct hg_info* info = margo_get_info(handle);
  assert(info);
  margo_instance_id mid = margo_hg_info_get_instance(info);
  assert(mid != MARGO_INSTANCE_NULL);
  
    
  /* Get input parameter in umap_server_read_rpc */
  umap_read_rpc_in_t input;
  ret = margo_get_input(handle, &input);
  if(ret != HG_SUCCESS){
    UMAP_ERROR("failed to get rpc intput");
  }

  UMAP_LOG(Debug, "request "<<input.id<<" of "<<input.size<<" bytes at offset "<< input.offset);
  
  /* the client signal termination
  * there is no built in functon in margo
  * to inform the server that all clients have completed
  */
  bool is_terminating = (input.size==0);
  if (is_terminating){

    num_completed_clients ++;
    goto fini;
    
  }else{

    /* Verify that the request is valid */
    char* server_buf_ptr = get_memory_object(input.id,
					     input.offset,
					     input.size);

    /* register memeory for bulk transfer */
    /* TODO: multiple bulk handlers might been */
    /*       created on overlapping memory regions */
    /*       Reuse bulk handle or merge multiple buffers into one bulk handle*/
    hg_bulk_t server_bulk_handle;
    void* server_buffer_ptr = server_buf_ptr + input.offset;
    void **buf_ptrs = (void **) &(server_buffer_ptr);
    ret = margo_bulk_create(mid,
			    1, buf_ptrs,&(input.size),
			    HG_BULK_READ_ONLY,
			    &server_bulk_handle);
    if(ret != HG_SUCCESS){
      UMAP_ERROR("Failed to create bulk handle on server");
    }

    UMAP_LOG(Debug,"start bulk transfer");
    /* initiate bulk transfer from server to client */
    /* margo_bulk_transfer is a blocking version of */
    /* that only returns when HG_Bulk_transfer complete */
    ret = margo_bulk_transfer(mid, HG_BULK_PUSH,
			      info->addr, input.bulk_handle, 0,  //client address, bulk handle, offset
			      server_bulk_handle, 0, input.size);//server bulk handle, offset, size of transfer
    if(ret != HG_SUCCESS){
      UMAP_ERROR("Failed to bulk transfer from server to client");
    }
    UMAP_LOG(Debug,"end bulk transfer");


    /* Inform the client side */
    umap_read_rpc_out_t output;
    output.ret  = RPC_RESPONSE_READ_DONE;
    ret = margo_respond(handle, &output);
    assert(ret == HG_SUCCESS);
    margo_bulk_free(server_bulk_handle);
  }

  
 fini:
    /* free margo resources */
    ret = margo_free_input(handle, &input);
    assert(ret == HG_SUCCESS);
    ret = margo_destroy(handle);
    assert(ret == HG_SUCCESS);
    UMAP_LOG(Debug, "Exiting");

    /* stop the server only if the clients specify the total number of clients */
    if(  num_clients>0 && num_completed_clients == num_clients)
      server_fini();

    return 0;
}
DEFINE_MARGO_RPC_HANDLER(umap_server_read_rpc)


/* 
 * The write rpc is executed on the server 
 * when the client request arrives
 * it starts bulk transfer from the client
 * to the server.
 */
static int umap_server_write_rpc(hg_handle_t handle)
{
  UMAP_LOG(Debug, "Entering");

  assert(mid != MARGO_INSTANCE_NULL);
  
  hg_return_t ret;

  /* get Mercury info */
  /* margo instance id is similar to mercury context */
  const struct hg_info* info = margo_get_info(handle);
  assert(info);
  margo_instance_id mid = margo_hg_info_get_instance(info);
  assert(mid != MARGO_INSTANCE_NULL);
  
    
  /* Get input parameter in umap_server_write_rpc */
  umap_write_rpc_in_t input;
  ret = margo_get_input(handle, &input);
  if(ret != HG_SUCCESS){
    UMAP_ERROR("failed to get rpc intput");
  }

  UMAP_LOG(Debug, "request to write "<<input.size<<" bytes at offset "<< input.offset);


  /* Shall we allow empty write request? or just ignore */
  assert(input.size>0);

  /* Verify that the request is valid */
  char* server_buf_ptr = get_memory_object(input.id,
					   input.offset,
					   input.size);
  
  /* register memeory for bulk transfer */
  /* TODO: multiple bulk handlers might been */
  /*       created on overlapping memory regions */
  /*       Reuse bulk handle or merge multiple buffers into one bulk handle*/
  hg_bulk_t server_bulk_handle;
  void* server_buffer_ptr = server_buf_ptr + input.offset;
  void **buf_ptrs = (void **) &(server_buffer_ptr);
  ret = margo_bulk_create(mid,
			  1, buf_ptrs,&(input.size),
			  HG_BULK_WRITE_ONLY,
			  &server_bulk_handle);
  if(ret != HG_SUCCESS){
    UMAP_ERROR("Failed to create bulk handle on server");
  }

  UMAP_LOG(Debug,"start bulk transfer");
  /* initiate bulk transfer from client to server */
  /* margo_bulk_transfer is a blocking version of */
  /* that only returns when HG_Bulk_transfer complete */
  ret = margo_bulk_transfer(mid, HG_BULK_PULL,
			    info->addr, input.bulk_handle, 0,  //client address, bulk handle, offset
			    server_bulk_handle, 0, input.size);//server bulk handle, offset, size of transfer
  if(ret != HG_SUCCESS){
    UMAP_ERROR("Failed to bulk transfer from client to server");
  }
  UMAP_LOG(Debug,"end bulk transfer");


  /* Inform the client side */
  umap_write_rpc_out_t output;
  output.ret  = RPC_RESPONSE_WRITE_DONE;
  ret = margo_respond(handle, &output);
  assert(ret == HG_SUCCESS);
  margo_bulk_free(server_bulk_handle);

  
 fini:
  /* free margo resources */
  ret = margo_free_input(handle, &input);
  assert(ret == HG_SUCCESS);
  ret = margo_destroy(handle);
  assert(ret == HG_SUCCESS);
  UMAP_LOG(Debug, "Exiting");
  
  
  return 0;
}
DEFINE_MARGO_RPC_HANDLER(umap_server_write_rpc)


/*                                                                                                                                                                              
 * The request rpc is executed on the server
 * when the client request arrives
 * it checks whether the requested memory resources
 * have been made available by the server
 */
static int umap_server_request_rpc(hg_handle_t handle)
{
  
  hg_return_t ret;

  /* get Mercury info */
  /* margo instance id is similar to mercury context */
  const struct hg_info* info = margo_get_info(handle);
  assert(info);
  /* TODO: is this check necessary? */
  margo_instance_id mid = margo_hg_info_get_instance(info);
  assert(mid != MARGO_INSTANCE_NULL);
      
  /* Get input parameter */
  umap_request_rpc_in_t in;
  ret = margo_get_input(handle, &in);
  if(ret != HG_SUCCESS){
    UMAP_ERROR("failed to get rpc intput");
  }
  UMAP_LOG(Info, " received a request ["<<in.id<<", "<<in.size<<"]" );


  umap_request_rpc_out_t output;
  /* Check whether the server has published the requested memory object */
  if( remote_memory_pool.find(in.id) != remote_memory_pool.end() ){

    /* Check whether the size match the record */
    /* TODO: shall we allow request with size smaller */
    if( in.size == remote_memory_pool[in.id].rsize ){
      output.ret  = RPC_RESPONSE_REQ_AVAIL;
    }else{
      output.ret  = RPC_RESPONSE_REQ_WRONG_SIZE;
      UMAP_LOG(Info, in.id << " on the Server has size="<<remote_memory_pool[in.id].rsize
	                   << ", but request size="<<in.size)
    }
  }else{
    output.ret  = RPC_RESPONSE_REQ_UNAVAIL;
    UMAP_LOG(Info, in.id << " has not been published by the Server");
  }

  /* Inform the client about the decison */
  ret = margo_respond(handle, &output);
  assert(ret == HG_SUCCESS);
  
  /* free margo resources */
  ret = margo_free_input(handle, &in);
  assert(ret == HG_SUCCESS);
  ret = margo_destroy(handle);
  assert(ret == HG_SUCCESS);
  
  return 0;
}
DEFINE_MARGO_RPC_HANDLER(umap_server_request_rpc)


static void setup_margo_server(){

  /* Init Margo using different transport protocols */
  int use_progress_thread = 1;//flag to use a dedicated thread for running Mercury's progress loop. 
  int rpc_thread_count = -1; //number of threads for running rpc calls
  mid = margo_init(PROTOCOL_MARGO_VERBS,
		   MARGO_SERVER_MODE,
		   use_progress_thread,
		   rpc_thread_count);
  if (mid == MARGO_INSTANCE_NULL) {
    UMAP_ERROR("margo_init protocol "<<PROTOCOL_MARGO_VERBS<<" failed");
  }
  UMAP_LOG(Info, "margo_init done");

  
  /* Find the address of this server */
  hg_addr_t addr;
  hg_return_t ret = margo_addr_self(mid, &addr);
  if (ret != HG_SUCCESS) {
    UMAP_ERROR("margo_addr_self failed");
    margo_finalize(mid);
  }

  /* Convert the server address to string*/
  char addr_string[128];
  hg_size_t addr_string_len = sizeof(addr_string);
  ret = margo_addr_to_string(mid, addr_string, &addr_string_len, addr);
  if (ret != HG_SUCCESS) {
    UMAP_ERROR("margo_addr_to_string failed");
    margo_addr_free(mid, addr);
    margo_finalize(mid);
  }
  UMAP_LOG(Info, "Margo RPC server: "<<addr_string);

  publish_server_addr(addr_string);
  
  margo_addr_free(mid, addr);
  
}


/*
 * Set the connection between servers
 * each peer server's margo address.
 */
void connect_margo_servers(void)
{
}


/*
 * Initialize a margo sever on the calling process
 */
void server_init()
{

  /* setup Margo RPC only if not done */
  if( mid != MARGO_INSTANCE_NULL ){
    UMAP_ERROR("Servers have been initialized before, returning...");
  }else{

    /* bootstraping to determine server and clients usnig MPI */
    /* not needed if MPI protocol is not used */
    int flag_mpi_initialized;
    MPI_Initialized(&flag_mpi_initialized);
    if( !flag_mpi_initialized )
      MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &server_id);
    

    setup_margo_server();
    if (mid == MARGO_INSTANCE_NULL) {
      UMAP_ERROR("cannot initialize Margo server");
    }

    //connect_margo_servers();

    /* register a remote read RPC */
    /* umap_rpc_in_t, umap_rpc_out_t are only significant on clients */
    /* uhg_umap_cb is only significant on the server */
    umap_read_rpc_id = MARGO_REGISTER(mid, "umap_read_rpc",
				       umap_read_rpc_in_t,
				       umap_read_rpc_out_t,
				       umap_server_read_rpc);

    umap_write_rpc_id = MARGO_REGISTER(mid, "umap_write_rpc",
				       umap_write_rpc_in_t,
				       umap_write_rpc_out_t,
				       umap_server_write_rpc);

    umap_request_rpc_id = MARGO_REGISTER(mid, "umap_request_rpc",
				       umap_request_rpc_in_t,
				       umap_request_rpc_out_t,
				       umap_server_request_rpc);

      
    /* init counters*/
    //num_clients = _num_clients;
    //num_completed_clients = 0;
    
    /* Two Options: (1) keep server active */
    /*              (2) shutdown when all clients complete*/
    //while (1) {
    //sleep(1);
    //}  
  }
}


void server_fini(void)
{
  
  UMAP_LOG(Info, "Server shutting down ...");

  /* shut down margo */
  if(mid!=MARGO_INSTANCE_NULL)
    margo_finalize(mid);

}
