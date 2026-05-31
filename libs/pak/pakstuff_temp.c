int PakLoadAnyFile(const char *filename, void **bufferptr) {
  if (g_bPK3) {
    PK3FileInfo *pInfo;

    // Convert incoming filename to unix-style lowercase for comparison
    char unixFile[WORK_LEN];
    __ConvertDOSToUnixName(unixFile, filename);
    strlwr(unixFile);

    // Walk the PK3 file list
    PK3List *pList = g_PK3Files.Next();
    while (pList != NULL) {
      pInfo = pList->Ptr();
      if (!pInfo || !pInfo->m_pName) {
        pList = pList->Next();
        continue;
      }

      qboolean match = qfalse;

      // 1. Try relative match
      if (stricmp(unixFile, pInfo->m_pName) == 0) {
        match = qtrue;
      }
      // 2. Try absolute match
      else if (pInfo->m_pBasePath) {
        char unixBase[WORK_LEN];
        __ConvertDOSToUnixName(unixBase, pInfo->m_pBasePath);
        strlwr(unixBase);
        int baseLen = strlen(unixBase);
        if (baseLen > 0 && unixBase[baseLen - 1] != '/') {
          strcat(unixBase, "/");
          baseLen++;
        }

        if (strnicmp(unixFile, unixBase, baseLen) == 0) {
          if (stricmp(unixFile + baseLen, pInfo->m_pName) == 0) {
            match = qtrue;
          }
        }
      }

      if (match) {
        if (!pInfo->m_zFile) {
          pList = pList->Next();
          continue;
        }
        unz_s savedState;
        memcpy(&savedState, pInfo->m_zFile, sizeof(unz_s));
        memcpy(pInfo->m_zFile, &pInfo->m_zInfo, sizeof(unz_s));

        if (unzOpenCurrentFile(pInfo->m_zFile) == UNZ_OK) {
          void *buffer = __qblockmalloc(pInfo->m_lSize + 1);
          int n = unzReadCurrentFile(pInfo->m_zFile, buffer, pInfo->m_lSize);
          *bufferptr = buffer;
          unzCloseCurrentFile(pInfo->m_zFile);

          memcpy(pInfo->m_zFile, &savedState, sizeof(unz_s));
          return n;
        }
        memcpy(pInfo->m_zFile, &savedState, sizeof(unz_s));
      }
      pList = pList->Next();
    }
    return -1;
  }

  for (int i = 0; i < dirsize; i++) {
    if (!stricmp(filename, pakdirptr[i].name)) {
      if (fseek(pakfile[m_nPAKIndex], pakdirptr[i].offset, SEEK_SET) >= 0) {
        void *buffer = __qmalloc(pakdirptr[i].size + 1);
        ((char *)buffer)[pakdirptr[i].size] = 0;
        if (fread(buffer, 1, pakdirptr[i].size, pakfile[m_nPAKIndex]) ==
            pakdirptr[i].size) {
          *bufferptr = buffer;
          return pakdirptr[i].size;
        }
      }
    }
  }
  return -1;
}