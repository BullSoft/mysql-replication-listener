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
#include <vector>
#include <stdexcept>

#include "row_of_fields.h"
#include "value.h"

using namespace mysql;

Row_of_fields& Row_of_fields::operator=(const Row_of_fields &right)
{
  if (size() != right.size()) {
      this->resize(right.size());
      //throw std::length_error("Row dimension doesn't match.");
  }
  int i= 0;
  // Because the parameter passed in is (const Row_of_fields &),
  // begin() will return a const_iterator
  for(std::vector<Value>::const_iterator it=right.begin(); it != right.end(); it++)
  {
    this->assign(++i, *it);
  }
  return *this;
}

Row_of_fields& Row_of_fields::operator=(Row_of_fields &right)
{
  if (size() != right.size()) {
      this->resize(right.size());
  }
  int i= 0;
  for(std::vector<Value>::iterator it=right.begin(); it != right.end(); it++)
  {
    this->assign(++i, *it);
  }
  return *this;
}
