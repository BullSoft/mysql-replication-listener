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

#ifndef _BOUNDED_BUFFER_H
#define	_BOUNDED_BUFFER_H

#include <deque>
#include <pthread.h>

template <class T>
class bounded_buffer
{
public:

  typedef std::deque<T> container_type;
  typedef typename container_type::size_type size_type;
  typedef typename container_type::value_type value_type;

  explicit bounded_buffer(size_type capacity) : m_unread(0), m_container(capacity), capacity(capacity)
  {
      pthread_mutex_init(&m_mutex, NULL);
      pthread_cond_init(&m_not_empty, NULL);
      pthread_cond_init(&m_not_full, NULL);
  }

  ~bounded_buffer()
  {
      pthread_mutex_destroy(&m_mutex);
      pthread_cond_destroy(&m_not_empty);
      pthread_cond_destroy(&m_not_full);
  }

  void push_front(const value_type& item)
  {
    pthread_mutex_lock(&m_mutex);
    if (m_unread == capacity)
        pthread_cond_wait(&m_not_full, &m_mutex);
    for (int i = m_unread; i > 0; i--) {
        m_container[i] = m_container[i-1];
    }
    m_container[0] = item;
    ++m_unread;
    pthread_mutex_unlock(&m_mutex);
    if (m_unread == 1)
        pthread_cond_signal(&m_not_empty);
  }

  void pop_back(value_type *pItem)
  {
    pthread_mutex_lock(&m_mutex);
    while (m_unread == 0)
        pthread_cond_wait(&m_not_empty, &m_mutex);
    *pItem = m_container[--m_unread];
    pthread_mutex_unlock(&m_mutex);
    if (m_unread == capacity -1)
        pthread_cond_signal(&m_not_full);
  }

  bool has_unread()
  {
    return is_not_empty();
  }

  void lock()
  {
      pthread_mutex_lock(&m_mutex);
  }

  void unlock()
  {
      pthread_mutex_unlock(&m_mutex);
  }

private:
  bounded_buffer(const bounded_buffer&);              // Disabled copy constructor
  bounded_buffer& operator = (const bounded_buffer&); // Disabled assign operator

  bool is_not_empty() const { return m_unread > 0; }
  bool is_not_full() const { return m_unread < capacity; }

  size_type capacity;
  size_type m_unread;
  container_type m_container;
  pthread_mutex_t m_mutex;
  pthread_cond_t m_not_empty;
  pthread_cond_t m_not_full;
};

#endif	/* _BOUNDED_BUFFER_H */

