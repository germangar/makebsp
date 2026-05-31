/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Foobar; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#ifndef __STR__
#define __STR__
//
// class Str
// loose replacement for CString from MFC
//
// #include "../common/cmdlib.h"
#include <string.h>

char *__StrDup(char *pStr);
char *__StrDup(const char *pStr);

class Str {
protected:
  bool m_bIgnoreCase;
  char *m_pStr;
  char *m_pWork;

public:
  Str() {
    m_bIgnoreCase = true;
    m_pStr = NULL;
    m_pWork = NULL;
  }

  Str(char *p) {
    m_bIgnoreCase = true;
    m_pStr = __StrDup(p);
    m_pWork = NULL;
  }

  Str(const char *p) {
    m_bIgnoreCase = true;
    m_pStr = __StrDup(p);
    m_pWork = NULL;
  }

  Str(const Str &rhs) {
    m_bIgnoreCase = rhs.m_bIgnoreCase;
    m_pStr = __StrDup(rhs.m_pStr);
    m_pWork = NULL;
  }

  void Deallocate() {
    delete[] m_pStr;
    m_pStr = NULL;
    delete[] m_pWork;
    m_pWork = NULL;
  }

  void Allocate(int n) {
    Deallocate();
    m_pStr = new char[n];
  }

  const char *GetBuffer() { return m_pStr; }

  void MakeEmpty() {
    Deallocate();
    m_pStr = __StrDup("");
  }

  ~Str() {
    Deallocate();
  }

  void MakeLower() {
    if (m_pStr) {
      strlwr(m_pStr);
    }
  }

  int Find(const char *p) {
    if (!m_pStr || !p) return -1;
    char *pf = strstr(m_pStr, p);
    return (pf) ? (int)(pf - m_pStr) : -1;
  }

  int GetLength() { return (m_pStr) ? (int)strlen(m_pStr) : 0; }

  const char *Left(int n) {
    delete[] m_pWork;
    m_pWork = NULL;
    if (n > 0) {
      m_pWork = new char[n + 1];
      strncpy(m_pWork, m_pStr, n);
      m_pWork[n] = '\0';
    } else {
      m_pWork = new char[1];
      m_pWork[0] = '\0';
    }
    return m_pWork;
  }

  const char *Right(int n) {
    delete[] m_pWork;
    m_pWork = NULL;
    if (n > 0) {
      m_pWork = new char[n + 1];
      int nStart = GetLength() - n;
      strncpy(m_pWork, &m_pStr[nStart], n);
      m_pWork[n] = '\0';
    } else {
      m_pWork = new char[1];
      m_pWork[0] = '\0';
    }
    return m_pWork;
  }

  char &operator*() { return *m_pStr; }
  char &operator*() const { return *const_cast<Str *>(this)->m_pStr; }
  operator void *() { return m_pStr; }
  operator char *() { return m_pStr; }
  operator const char *() { return reinterpret_cast<const char *>(m_pStr); }
  operator unsigned char *() {
    return reinterpret_cast<unsigned char *>(m_pStr);
  }
  operator const unsigned char *() {
    return reinterpret_cast<const unsigned char *>(m_pStr);
  }
  Str &operator=(const Str &rhs) {
    if (&rhs != this) {
      Deallocate();
      m_bIgnoreCase = rhs.m_bIgnoreCase;
      m_pStr = __StrDup(rhs.m_pStr);
    }
    return *this;
  }

  Str &operator=(const char *pStr) {
    if (m_pStr != pStr) {
      Deallocate();
      m_pStr = __StrDup(pStr);
    }
    return *this;
  }

  Str &operator+=(const char *pStr) {
    if (pStr) {
      if (m_pStr) {
        char *p = new char[strlen(m_pStr) + strlen(pStr) + 1];
        strcpy(p, m_pStr);
        strcat(p, pStr);
        delete[] m_pStr;
        m_pStr = p;
      } else {
        m_pStr = __StrDup(pStr);
      }
    }
    return *this;
  }

  Str &operator+=(const char c) {
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    return operator+=(buf);
  }

  bool operator==(const Str &rhs) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, rhs.m_pStr) == 0
                           : strcmp(m_pStr, rhs.m_pStr) == 0;
  }
  bool operator==(char *pStr) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, pStr) == 0
                           : strcmp(m_pStr, pStr) == 0;
  }
  bool operator==(const char *pStr) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, pStr) == 0
                           : strcmp(m_pStr, pStr) == 0;
  }
  bool operator!=(Str &rhs) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, rhs.m_pStr) != 0
                           : strcmp(m_pStr, rhs.m_pStr) != 0;
  }
  bool operator!=(char *pStr) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, pStr) != 0
                           : strcmp(m_pStr, pStr) != 0;
  }
  bool operator!=(const char *pStr) const {
    return (m_bIgnoreCase) ? stricmp(m_pStr, pStr) != 0
                           : strcmp(m_pStr, pStr) != 0;
  }
  char &operator[](int nIndex) { return m_pStr[nIndex]; }
  char &operator[](int nIndex) const { return m_pStr[nIndex]; }
};

#endif