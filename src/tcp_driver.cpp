/*
Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights
reserved.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of
the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
02110-1301  USA
*/

#include "binlog_api.h"

#include <iostream>
#include <fstream>
#include <time.h>
#include <stdint.h>
#include <streambuf>
#include <stdio.h>
#include <functional>
#include <pthread.h>
#include <exception>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "tcp_driver.h"
#include "protocol.h"
#include "binlog_event.h"
#include "rowset.h"
#include "field_iterator.h"

using asio::ip::tcp;
using namespace mysql::system;
using namespace mysql;

typedef unsigned char uchar;

namespace mysql { namespace system {

static int encrypt_password(uint8_t *reply,   /* buffer at least EVP_MAX_MD_SIZE */
                            const uint8_t *scramble_buff,
                            const char *pass);
static int hash_sha1(uint8_t *output, ...);

    int Binlog_tcp_driver::connect(const std::string& user, const std::string& passwd,
                                   const std::string& host, long port,
                                   const std::string& binlog_filename, size_t offset)
{
  m_user=user;
  m_passwd=passwd;
  m_host=host;
  m_port=port;

  if (!m_socket)
  {
    if ((m_socket=sync_connect_and_authenticate(m_io_service, user, passwd, host, port)) == 0)
      return 1;
  }

  /**
   * Get the master status if we don't know the name of the file.
   */
  if (binlog_filename == "")
  {
    if (fetch_master_status(m_socket, &m_binlog_file_name, &m_binlog_offset))
      return 1;
  } else
  {
    m_binlog_file_name=binlog_filename;
    m_binlog_offset=offset;
  }


  /* We're ready to start the io service and request the binlog dump. */
  start_binlog_dump(m_binlog_file_name, m_binlog_offset);

  return 0;
}

tcp::socket *sync_connect_and_authenticate(asio::io_service &io_service, const std::string &user, const std::string &passwd, const std::string &host, long port)
{

  tcp::resolver resolver(io_service);
  tcp::resolver::query query(host.c_str(), "0");

  asio::error_code error = asio::error::host_not_found;

  if (port == 0)
    port= 3306;

  tcp::socket *socket=new tcp::socket(io_service);
  /*
    Try each endpoint until we successfully establish a connection.
   */
  try {
  tcp::resolver::iterator endpoint_iterator=resolver.resolve(query);
  tcp::resolver::iterator end;

  while (error && endpoint_iterator != end)
  {
    /*
      Hack to set port number from a long int instead of a service.
     */
    tcp::endpoint endpoint=endpoint_iterator->endpoint();
    endpoint.port(port);

    socket->close();
    socket->connect(endpoint, error);
    endpoint_iterator++;
  }
  } catch(...)
  {
    return 0;
  }

  if (error)
  {
    return 0;
  }


  /*
   * Successfully connected to the master.
   * 1. Accept handshake from server
   * 2. Send authentication package to the server
   * 3. Accept OK server package (or error in case of failure)
   * 4. Send COM_REGISTER_SLAVE command to server
   * 5. Accept OK package from server
   */

  asio::streambuf server_messages;

  /*
   * Get package header
   */
  unsigned long packet_length;
  unsigned char packet_no;
  if (proto_read_package_header(socket, server_messages, &packet_length, &packet_no))
  {
    return 0;
  }

  /*
   * Get server handshake package
   */
  std::streamsize inbuffer=server_messages.in_avail();
  if (inbuffer < 0)
    inbuffer=0;
  asio::read(*socket, server_messages, asio::transfer_at_least(packet_length - inbuffer));
  std::istream server_stream(&server_messages);

  struct st_handshake_package handshake_package;

  proto_get_handshake_package(server_stream, handshake_package, packet_length);

  if (authenticate(socket, user, passwd, handshake_package))
    return 0;

  /*
   * Register slave to master
   */
  std::ostream command_request_stream(&server_messages);

  Protocol_chunk<uint8_t> prot_command(COM_REGISTER_SLAVE);
  Protocol_chunk<uint16_t> prot_connection_port(port);
  Protocol_chunk<uint32_t> prot_rpl_recovery_rank(0);
  Protocol_chunk<uint32_t> prot_server_id(1);
  Protocol_chunk<uint32_t> prot_master_server_id(1);

  Protocol_chunk<uint8_t> prot_report_host_strlen(host.size());
  Protocol_chunk<uint8_t> prot_user_strlen(user.size());
  Protocol_chunk<uint8_t> prot_passwd_strlen(passwd.size());

  command_request_stream << prot_command
          << prot_server_id
          << prot_report_host_strlen
          << host
          << prot_user_strlen
          << user
          << prot_passwd_strlen
          << passwd
          << prot_connection_port
          << prot_rpl_recovery_rank
          << prot_master_server_id;

  int size=server_messages.size();
  char command_packet_header[4];
  try {
    write_packet_header(command_packet_header, size, 0); // packet_no= 0

    // Send the request.
    asio::write(*socket,
                       asio::buffer(command_packet_header, 4),
                       asio::transfer_at_least(4));
    asio::write(*socket, server_messages,
                       asio::transfer_at_least(size));
  } catch( asio::error_code e)
  {
    return 0;
  }

  // Get Ok-package
  packet_length=proto_get_one_package(socket, server_messages, &packet_no);

  std::istream cmd_response_stream(&server_messages);

  uint8_t result_type;
  Protocol_chunk<uint8_t> prot_result_type(result_type);

  cmd_response_stream >> prot_result_type;

  if (result_type == 0)
  {
    struct st_ok_package ok_package;
    prot_parse_ok_message(cmd_response_stream, ok_package, packet_length);
  } else
  {
    struct st_error_package error_package;
    prot_parse_error_message(cmd_response_stream, error_package, packet_length);
    return 0;
  }

  return socket;
}

void Binlog_tcp_driver::start_binlog_dump(const std::string &binlog_file_name, size_t offset)
{
  asio::streambuf server_messages;
  this->thread_data = (struct Thread_data *)malloc(sizeof(struct Thread_data));

  std::ostream command_request_stream(&server_messages);

  Protocol_chunk<uint8_t>  prot_command(COM_BINLOG_DUMP);
  Protocol_chunk<uint32_t> prot_binlog_offset(offset); // binlog position to start at
  Protocol_chunk<uint16_t> prot_binlog_flags(0); // not used
  Protocol_chunk<uint32_t> prot_server_id(1); // must not be 0; see handshake package

  command_request_stream
          << prot_command
          << prot_binlog_offset
          << prot_binlog_flags
          << prot_server_id
          << binlog_file_name;

  int size=server_messages.size();
  char command_packet_header[4];
  write_packet_header(command_packet_header, size, 0);

  // Send the request.
  asio::write(*m_socket,
                     asio::buffer(command_packet_header, 4),
                     asio::transfer_at_least(4));
  asio::write(*m_socket, server_messages,
                     asio::transfer_at_least(size));

  /*
   Start receiving binlog events.
   */
  if (!m_shutdown) {
      Read_handler read_handler;
      read_handler.method     = &Binlog_tcp_driver::handle_net_packet_header;
      read_handler.tcp_driver = this;
      asio::async_read(*m_socket, asio::buffer(m_net_header, 4),
          read_handler);
  }

  /*
   Start the event loop in a new thread
   */
  if (!m_event_loop) {
      this->thread_data->tcp_driver = this;
      m_event_loop = (pthread_t *)malloc(sizeof(pthread_t));
      pthread_create(m_event_loop, NULL, &Binlog_tcp_driver::start, (void *)this->thread_data);
  }
}

/**
 Helper function used to extract the event header from a memory block
 */
static void proto_event_packet_header(asio::streambuf &event_src, Log_event_header *h)
{
  std::istream is(&event_src);

  Protocol_chunk<uint8_t> prot_marker(h->marker);
  Protocol_chunk<uint32_t> prot_timestamp(h->timestamp);
  Protocol_chunk<uint8_t> prot_type_code(h->type_code);
  Protocol_chunk<uint32_t> prot_server_id(h->server_id);
  Protocol_chunk<uint32_t> prot_event_length(h->event_length);
  Protocol_chunk<uint32_t> prot_next_position(h->next_position);
  Protocol_chunk<uint16_t> prot_flags(h->flags);

  is >> prot_marker
          >> prot_timestamp
          >> prot_type_code
          >> prot_server_id
          >> prot_event_length
          >> prot_next_position
          >> prot_flags;
}

void Binlog_tcp_driver::handle_net_packet(const asio::error_code& err, std::size_t bytes_transferred)
{
  if (err)
  {
    Binary_log_event * ev= create_incident_event(175, err.message().c_str(), m_binlog_offset);
    std::cout << "1:" << err.message() << std::endl;
    m_event_queue->push_front(ev);
    return;
  }

  if (bytes_transferred > MAX_PACKAGE_SIZE || bytes_transferred == 0)
  {
    std::ostringstream os;
    os << "Expected byte size to be between 0 and "
       << MAX_PACKAGE_SIZE
       << " number of bytes; got "
       << bytes_transferred
       << " instead.";
    Binary_log_event * ev= create_incident_event(175, os.str().c_str(), m_binlog_offset);
    std::cout << "2:" << os.str() << std::endl;
    m_event_queue->push_front(ev);
    return;
  }

  //assert(m_waiting_event != 0);
  //std::cerr << "Committing '"<< bytes_transferred << "' bytes to the event stream." << std::endl;
  m_event_stream_buffer.commit(bytes_transferred);
  /*
    If the event object doesn't have an event length it means that the header
    hasn't been parsed. If the event stream also contains enough bytes
    we make the assumption that the next bytes waiting in the stream is
    the event header and attempt to parse it.
   */
  if (m_waiting_event->event_length == 0 && m_event_stream_buffer.size() >= 19)
  {
    /*
      Copy and remove from the event stream, the remaining bytes might be
      dynamic payload.
     */
    //std::cerr << "Consuming event stream for header. Size before: " << m_event_stream_buffer.size() << std::endl;
    proto_event_packet_header(m_event_stream_buffer, m_waiting_event);
    //std::cerr << " Size after: " << m_event_stream_buffer.size() << std::endl;
  }

  //std::cerr << "Event length: " << m_waiting_event->header()->event_length << " and available payload size is " << m_event_stream_buffer.size()+LOG_EVENT_HEADER_SIZE-1 <<  std::endl;
  if (m_waiting_event->event_length == m_event_stream_buffer.size() + LOG_EVENT_HEADER_SIZE - 1)
  {
    /*
     If the header length equals the size of the payload plus the
     size of the header, the event object is complete.
     Next we need to parse the payload buffer
     */
    std::istream is(&m_event_stream_buffer);
    Binary_log_event * event= parse_event(is, m_waiting_event);

    m_event_stream_buffer.consume(m_event_stream_buffer.size());

    m_event_queue->push_front(event);

    /*
      Note on memory management: The pushed Binary_log_event will be
      deleted in user land.
    */
    delete m_waiting_event;
    m_waiting_event= 0;
  }

  if (!m_shutdown) {
      Read_handler read_handler;
      read_handler.method     = &Binlog_tcp_driver::handle_net_packet_header;
      read_handler.tcp_driver = this;
      asio::async_read(*m_socket, asio::buffer(m_net_header, 4),
          read_handler);
  }
}

void Binlog_tcp_driver::handle_net_packet_header(const asio::error_code& err, std::size_t bytes_transferred)
{
  if (err)
  {
    Binary_log_event * ev= create_incident_event(175, err.message().c_str(), m_binlog_offset);
    std::cout << "3:" << err.message() << std::endl;
    m_event_queue->push_front(ev);
    return;
  }

  if (bytes_transferred != 4)
  {
    std::ostringstream os;
    os << "Expected byte size to be between 0 and "
       << MAX_PACKAGE_SIZE
       << " number of bytes; got "
       << bytes_transferred
       << " instead.";
    Binary_log_event * ev= create_incident_event(175, os.str().c_str(), m_binlog_offset);
    std::cout << "4:" << os.str() << std::endl;
    m_event_queue->push_front(ev);
    return;
  }

  int packet_length=(unsigned long) (m_net_header[0] &0xFF);
  packet_length+=(unsigned long) ((m_net_header[1] &0xFF) << 8);
  packet_length+=(unsigned long) ((m_net_header[2] &0xFF) << 16);

  // TODO validate packet sequence numbers
  //int packet_no=(unsigned char) m_net_header[3];

  if (m_waiting_event == 0)
  {
    //std::cerr << "event_stream_buffer.size= " << m_event_stream_buffer.size() << std::endl;
    m_waiting_event= new Log_event_header();
    m_event_packet=  asio::buffer_cast<char *>(m_event_stream_buffer.prepare(packet_length));
    //assert(m_event_stream_buffer.size() == 0);
  }


  Read_handler read_handler;
  read_handler.method     = &Binlog_tcp_driver::handle_net_packet;
  read_handler.tcp_driver = this;
  asio::async_read(*m_socket,
                          asio::buffer(m_event_packet, packet_length),
                          read_handler);
}

    int authenticate(tcp::socket *socket, const std::string& user, const std::string& passwd,
                     const st_handshake_package &handshake_package)
{
  try
  {
    /*
     * Send authentication package
     */
    asio::streambuf auth_request_header;
    asio::streambuf auth_request;
    std::string database("mysql"); // 0 terminated


    std::ostream request_stream(&auth_request);

    uint8_t filler_buffer[23];
    memset((char *) filler_buffer, '\0', 23);

    uint8_t reply[EVP_MAX_MD_SIZE];
    memset(reply, '\0', EVP_MAX_MD_SIZE);
    uint8_t scramble_buff[21];
    memcpy(scramble_buff, handshake_package.scramble_buff, 8);
    memcpy(scramble_buff+8, handshake_package.scramble_buff2, 13);
    int passwd_length= 0;
    if (passwd.size() > 0)
      passwd_length= encrypt_password(reply, scramble_buff, passwd.c_str());

    Protocol_chunk<uint32_t> prot_client_flags((uint32_t) CLIENT_BASIC_FLAGS);
    Protocol_chunk<uint32_t> prot_max_packet_size(MAX_PACKAGE_SIZE);
    Protocol_chunk<uint8_t>  prot_charset_number(handshake_package.server_language);
    Protocol_chunk<uint8_t>  prot_filler_buffer(filler_buffer, 23);
    Protocol_chunk<uint8_t>  prot_scramble_buffer_size((uint8_t) passwd_length);
    Protocol_chunk<uint8_t>  prot_scamble_buffer((uint8_t *)reply, passwd_length);

    request_stream << prot_client_flags
                   << prot_max_packet_size
                   << prot_charset_number
                   << prot_filler_buffer
                   << user << '\0'
                   << prot_scramble_buffer_size
                   << prot_scamble_buffer
                   << database << '\0';


    int size=auth_request.size();
    char auth_packet_header[4];

    write_packet_header(auth_packet_header, size, 1);

    /*
     *  Send the request.
     */
    asio::write(*socket, asio::buffer(auth_packet_header, 4),
                       asio::transfer_at_least(4));
    asio::write(*socket, auth_request,
                       asio::transfer_at_least(size));

    /*
     * Get server authentication response
     */
    unsigned long packet_length;
    unsigned char packet_no=1;
    packet_length=proto_get_one_package(socket, auth_request, &packet_no);

    std::istream auth_response_stream(&auth_request);

    uint8_t result_type;
    Protocol_chunk<uint8_t> prot_result_type(result_type);


    auth_response_stream >> prot_result_type;

    if (result_type == 0)
    {
      struct st_ok_package ok_package;
      prot_parse_ok_message(auth_response_stream, ok_package, packet_length);
    } else
    {
      struct st_error_package error_package;
      prot_parse_error_message(auth_response_stream, error_package, packet_length);
      return 1;
    }

    return 0;
  } catch (asio::system_error e)
  {
    // TODO log error; adjust return code
    return 1;
  }
}

int Binlog_tcp_driver::wait_for_next_event(mysql::Binary_log_event **event_ptr)
{
  // poll for new event until one event is found.
  // return the event
  if (event_ptr)
    *event_ptr = 0;
  m_event_queue->pop_back(event_ptr);
  return 0;
}

void *Binlog_tcp_driver::start(void *data)
{
   Thread_data *thread_data = (Thread_data *)data;
   thread_data->tcp_driver->start_event_loop();
   return NULL;
}

void Binlog_tcp_driver::start_event_loop()
{
  while (true)
  {
    asio::error_code err;
    int executed_jobs=m_io_service.run(err);
    if (err)
    {
      // TODO what error appear here?
    }

    /*
      This function must be called prior to any second or later set of
      invocations of the run(), run_one(), poll() or poll_one() functions when
      a previous invocation of these functions returned due to the io_service
      being stopped or running out of work. This function allows the io_service
      to reset any internal state, such as a "stopped" flag.
    */
    m_io_service.reset();

    /*
      Don't shutdown until the io service has reset!
    */
    if (m_shutdown)
    {
      m_shutdown= false;
      break;
    }

    reconnect();
  }

}

int Binlog_tcp_driver::connect()
{
  return connect(m_user, m_passwd, m_host, m_port);
}

/**
 * Make synchronous reconnect.
 */
void Binlog_tcp_driver::reconnect()
{
  disconnect();
  connect(m_user, m_passwd, m_host, m_port);
}

void Binlog_tcp_driver::disconnect()
{
  Binary_log_event * event;
  m_waiting_event= 0;
  m_event_stream_buffer.consume(m_event_stream_buffer.in_avail());
  while(m_event_queue->has_unread())
  {
    m_event_queue->pop_back(&event);
    delete(event);
  }
  if (m_socket)
    m_socket->close();
  m_socket= 0;
}


void Binlog_tcp_driver::shutdown(void)
{
  m_shutdown= true;
  m_io_service.stop();
}

int Binlog_tcp_driver::set_position(const std::string &str, unsigned long position)
{
  /*
    Validate the new position before we attempt to set. Once we set the
    position we won't know if it succeded because the binlog dump is
    running in another thread asynchronously.
  */
  asio::io_service io_service;
  tcp::socket *socket;

  if ((socket= sync_connect_and_authenticate(io_service, m_user, m_passwd, m_host, m_port)) == 0)
    return ERR_FAIL;

  std::map<std::string, unsigned long > binlog_map;
  fetch_binlogs_name_and_size(socket, binlog_map);
  socket->close();
  delete socket;

  std::map<std::string, unsigned long >::iterator binlog_itr= binlog_map.find(str);

  /*
    If the file name isn't listed on the server we will fail here.
  */
  if (binlog_itr == binlog_map.end())
    return ERR_FAIL;

  /*
    If the requested position is greater than the file size we will fail
    here.
  */
  if (position > binlog_itr->second)
    return ERR_FAIL;


  /*
    By posting to the io service we guarantee that the operations are
    executed in the same thread as the io_service is running in.
  */
  Shutdown_handler shutdown_handler;
  shutdown_handler.method     = &Binlog_tcp_driver::shutdown;
  shutdown_handler.tcp_driver = this;
  m_io_service.post(shutdown_handler);
  if (m_event_loop)
  {
    pthread_join(*m_event_loop, NULL);
    free(m_event_loop);
  }
  m_event_loop= 0;
  disconnect();
  /*
    Uppon return of connect we only know if we succesfully authenticated
    against the server. The binlog dump command is executed asynchronously
    in another thread.
  */
  if (connect(m_user, m_passwd, m_host, m_port, str, position) == 0)
    return ERR_OK;
  else
    return ERR_FAIL;
}

int Binlog_tcp_driver::get_position(std::string *filename_ptr, unsigned long *position_ptr)
{
  asio::io_service io_service;

  tcp::socket *socket;

  if ((socket=sync_connect_and_authenticate(io_service, m_user, m_passwd, m_host, m_port)) == 0)
    return ERR_FAIL;

  if (fetch_master_status(socket, &m_binlog_file_name, &m_binlog_offset))
    return ERR_FAIL;

  socket->close();
  delete socket;
  if (filename_ptr)
    *filename_ptr= m_binlog_file_name;
  if (position_ptr)
    *position_ptr= m_binlog_offset;
  return ERR_OK;
}

bool fetch_master_status(tcp::socket *socket, std::string *filename, unsigned long *position)
{
  asio::streambuf server_messages;

  std::ostream command_request_stream(&server_messages);

  Protocol_chunk<uint8_t> prot_command(COM_QUERY);

  command_request_stream << prot_command
          << "SHOW MASTER STATUS";

  int size=server_messages.size();
  char command_packet_header[4];
  write_packet_header(command_packet_header, size, 0);

  // Send the request.
  asio::write(*socket, asio::buffer(command_packet_header, 4), asio::transfer_at_least(4));
  asio::write(*socket, server_messages, asio::transfer_at_least(size));

  Result_set result_set(socket);

  Converter conv;
  for(Result_set::iterator it = result_set.begin();
          it != result_set.end();
          it++)
  {
    Row_of_fields row(*it);
    *filename= "";
    conv.to(*filename, row[0]);
    long pos;
    conv.to(pos, row[1]);
    *position= (unsigned long)pos;
  }
  return false;
}

bool fetch_binlogs_name_and_size(tcp::socket *socket, std::map<std::string, unsigned long> &binlog_map)
{
  asio::streambuf server_messages;

  std::ostream command_request_stream(&server_messages);

  Protocol_chunk<uint8_t> prot_command(COM_QUERY);

  command_request_stream << prot_command
          << "SHOW BINARY LOGS";

  int size=server_messages.size();
  char command_packet_header[4];
  write_packet_header(command_packet_header, size, 0);

  // Send the request.
  asio::write(*socket, asio::buffer(command_packet_header, 4), asio::transfer_at_least(4));
  asio::write(*socket, server_messages, asio::transfer_at_least(size));

  Result_set result_set(socket);

  Converter conv;
  for(Result_set::iterator it = result_set.begin();
          it != result_set.end();
          it++)
  {
    Row_of_fields row(*it);
    std::string filename;
    long position;
    conv.to(filename, row[0]);
    conv.to(position, row[1]);
    binlog_map.insert(std::make_pair<std::string, unsigned long>(filename, (unsigned long)position));
  }
  return false;
}


#define SCRAMBLE_BUFF_SIZE 20

int hash_sha1(uint8_t *output, ...)
{
  /* size at least EVP_MAX_MD_SIZE */
  va_list ap;
  size_t result;
  EVP_MD_CTX *hash_context = EVP_MD_CTX_create();

  va_start(ap, output);
  EVP_DigestInit_ex(hash_context, EVP_sha1(), NULL);
  while ( 1 )
  {
    const uint8_t *data = va_arg(ap, const uint8_t *);
    int length = va_arg(ap, int);
    if ( length < 0 )
      break;
    EVP_DigestUpdate(hash_context, data, length);
  }
  EVP_DigestFinal_ex(hash_context, (unsigned char *)output, (unsigned int *)&result);
  va_end(ap);
  return result;
}


int encrypt_password(uint8_t *reply,   /* buffer at least EVP_MAX_MD_SIZE */
	                   const uint8_t *scramble_buff,
		                 const char *pass)
{
  uint8_t hash_stage1[EVP_MAX_MD_SIZE], hash_stage2[EVP_MAX_MD_SIZE];
  //EVP_MD_CTX *hash_context = EVP_MD_CTX_create();

  /* Hash password into hash_stage1 */
  int length_stage1 = hash_sha1(hash_stage1,
                                pass, strlen(pass),
                                NULL, -1);

  /* Hash hash_stage1 into hash_stage2 */
  int length_stage2 = hash_sha1(hash_stage2,
                                hash_stage1, length_stage1,
                                NULL, -1);

  int length_reply = hash_sha1(reply,
                               scramble_buff, SCRAMBLE_BUFF_SIZE,
                               hash_stage2, length_stage2,
                               NULL, -1);

  //assert(length_reply <= EVP_MAX_MD_SIZE);
  //assert(length_reply == length_stage1);

  int i;
  for ( i=0 ; i<length_reply ; ++i )
    reply[i] = hash_stage1[i] ^ reply[i];
  return length_reply;
}

}} // end namespace mysql::system
