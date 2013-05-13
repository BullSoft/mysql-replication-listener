#include "binlog_api.h"

/**
  @file basic-1
  @author Mats Kindahl <mats.kindahl@oracle.com>

  modified by ideal <idealities@gmail.com>

  This is a basic example that just opens a binary log either from a
  file or a server and print out what events are found.  It uses a
  simple event loop and checks information in the events using a
  switch.
 */

// Actually namespace mysql has been using in many headers like rowset.h
// and rowset.h is included by binlog_api.h 。。。。
//using mysql::Binary_log;
using mysql::system::create_transport;

int main(int argc, char** argv) {

  if (argc != 2) {
    std::cerr << "Usage: basic-1 <uri>" << std::endl;
    exit(2);
  }

  Binary_log binlog(create_transport(argv[1]));
  binlog.connect();
  
  Converter converter;

  Binary_log_event *event;
  Table_map_event  *tmev;

  while (true) {
    int result = binlog.wait_for_next_event(&event);
    if (result == ERR_EOF)
      break;
    std::cout << "Found event of type "
              << event->get_event_type();
    int event_type = event->get_event_type();
    if (event_type == mysql::TABLE_MAP_EVENT) {
        tmev = (mysql::Table_map_event *)event;
    }
    if (event_type == mysql::WRITE_ROWS_EVENT) {
        mysql::Row_event *row_event = (mysql::Row_event *)event;
        mysql::Row_event_set rows(row_event, tmev);
        mysql::Row_event_set::iterator itor = rows.begin();
        do {
            mysql::Row_of_fields fields = *itor;
            mysql::Row_of_fields::iterator it = fields.begin();
            do {
                std::string out;
                converter.to(out, *it);
                std::cout << "\t" << out;
            } while(++it != fields.end());
        } while (++itor != rows.end());
    }
    std::cout << std::endl;
  }
}
