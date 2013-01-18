/************ Odbconn C++ Functions Source Code File (.CPP) ************/
/*  Name: ODBCONN.CPP  Version 1.5                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1998-2012    */
/*                                                                     */
/*  This file contains the ODBC connection classes functions.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
//nclude <io.h>
//nclude <fcntl.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else
#if defined(UNIX)
#include <errno.h>
#else
//nclude <io.h>
#endif
//nclude <fcntl.h>
#define NODW
#endif

/***********************************************************************/
/*  Required objects includes.                                         */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
//#include "kindex.h"
#include "xtable.h"
#include "tabodbc.h"
#include "plgcnx.h"                       // For DB types
#include "resource.h"
#include "valblk.h"


#if defined(WIN32)
/***********************************************************************/
/*  For dynamic load of ODBC32.DLL                                     */
/***********************************************************************/
#pragma comment(lib, "odbc32.lib")
extern "C" HINSTANCE s_hModule;           // Saved module handle
#else  // !WIN32
extern "C" int GetRcString(int id, char *buf, int bufsize);
#endif // !WIN32

/***********************************************************************/
/*  Some macro's (should be defined elsewhere to be more accessible)   */
/***********************************************************************/
#if defined(_DEBUG)
#define ASSERT(f)          assert(f)
#define DEBUG_ONLY(f)      (f)
#else   // !_DEBUG
#define ASSERT(f)          ((void)0)
#define DEBUG_ONLY(f)      ((void)0)
#endif  // !_DEBUG

/***********************************************************************/
/*  ODBConn static members initialization.                             */
/***********************************************************************/
  HENV ODBConn::m_henv = SQL_NULL_HENV;
  int  ODBConn::m_nAlloc = 0;        // per-Appl reference to HENV above

/**************************************************************************/
/*  Allocate the result structure that will contain result data.          */
/**************************************************************************/
PQRYRES PlgAllocResult(PGLOBAL g, int ncol, int maxres, int ids,
                       int *dbtype, int *buftyp, unsigned int *length,
                       bool blank = false, bool nonull = false)
  {
  char     cname[NAM_LEN+1];
  int      i;
  PCOLRES *pcrp, crp;
  PQRYRES  qrp;

  /************************************************************************/
  /*  Allocate the structure used to contain the result set.              */
  /************************************************************************/
  qrp = (PQRYRES)PlugSubAlloc(g, NULL, sizeof(QRYRES));
  pcrp = &qrp->Colresp;
  qrp->Continued = false;
  qrp->Truncated = false;
  qrp->Info = false;
  qrp->Suball = true;
  qrp->Maxres = maxres;
  qrp->Maxsize = 0;
  qrp->Nblin = 0;
  qrp->Nbcol = 0;                                     // will be ncol
  qrp->Cursor = 0;
  qrp->BadLines = 0;

  for (i = 0; i < ncol; i++) {
    *pcrp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
    crp = *pcrp;
    pcrp = &crp->Next;
    crp->Colp = NULL;
    crp->Ncol = ++qrp->Nbcol;
    crp->Type = buftyp[i];
    crp->Length = length[i];
    crp->Clen = GetTypeSize(crp->Type, length[i]);
    crp->Prec = 0;
    crp->DBtype = dbtype[i];

    if (ids > 0) {
#if defined(XMSG)
      // Get header from message file
			strncpy(cname, PlugReadMessage(g, ids + crp->Ncol, NULL), NAM_LEN);
			cname[NAM_LEN] = 0;					// for truncated long names
#elif defined(WIN32)
      // Get header from ressource file
      LoadString(s_hModule, ids + crp->Ncol, cname, sizeof(cname));
#else   // !WIN32
      GetRcString(ids + crp->Ncol, cname, sizeof(cname));
#endif  // !WIN32
      crp->Name = (PSZ)PlugSubAlloc(g, NULL, strlen(cname) + 1);
      strcpy(crp->Name, cname);
    } else
      crp->Name = NULL;           // Will be set by caller

    // Allocate the Value Block that will contain data
    if (crp->Length || nonull)
      crp->Kdata = AllocValBlock(g, NULL, crp->Type, maxres,
                                          crp->Length, 0, true, blank);
    else
      crp->Kdata = NULL;

    if (g->Trace)
      htrc("Column(%d) %s type=%d len=%d value=%p\n",
              crp->Ncol, crp->Name, crp->Type, crp->Length, crp->Kdata);

    } // endfor i

  *pcrp = NULL;

  return qrp;
  } // end of PlgAllocResult

/***********************************************************************/
/*  Allocate the structure used to refer to the result set.            */
/***********************************************************************/
CATPARM *AllocCatInfo(PGLOBAL g, CATINFO fid, char *tab, PQRYRES qrp)
  {
  size_t   i, m, n;
  CATPARM *cap;

#if defined(_DEBUG)
  assert(qrp);
#endif

  m = (size_t)qrp->Maxres;
  n = (size_t)qrp->Nbcol;
  cap = (CATPARM *)PlugSubAlloc(g, NULL, sizeof(CATPARM));
  memset(cap, 0, sizeof(CATPARM));
  cap->Id = fid;
  cap->Qrp = qrp;
  cap->Tab = (PUCHAR)tab;
  cap->Vlen = (SQLLEN* *)PlugSubAlloc(g, NULL, n * sizeof(SDWORD *));

  for (i = 0; i < n; i++)
    cap->Vlen[i] = (SQLLEN *)PlugSubAlloc(g, NULL, m * sizeof(SDWORD));

  cap->Status = (UWORD *)PlugSubAlloc(g, NULL, m * sizeof(UWORD));
  return cap;
  } // end of AllocCatInfo

/***********************************************************************/
/*  Check for nulls and reset them to Null (?) values.                 */
/***********************************************************************/
void ResetNullValues(CATPARM *cap)
  {
  int      i, n, ncol;
  PCOLRES  crp;
  PQRYRES  qrp = cap->Qrp;

#if defined(_DEBUG)
  assert(qrp);
#endif

  ncol = qrp->Nbcol;

  for (i = 0, crp = qrp->Colresp; i < ncol && crp; i++, crp = crp->Next)
    for (n = 0; n < qrp->Nblin; n++)
      if (cap->Vlen[i][n] == SQL_NULL_DATA)
        crp->Kdata->Reset(n);

  } // end of ResetNullValues

/***********************************************************************/
/*  ODBCTables: constructs the result blocks containing all tables in  */
/*  an ODBC database that will be retrieved by GetData commands.       */
/*  Note: The first two columns (Qualifier, Owner) are ignored.        */
/***********************************************************************/
PQRYRES ODBCTables(PGLOBAL g, ODBConn *op, char *dsn, char *tabpat,
                                                      char *tabtyp)
  {
  static int dbtype[] = {DB_CHAR, DB_CHAR, DB_CHAR, DB_CHAR};
  static int buftyp[] = {TYPE_STRING, TYPE_STRING,
                         TYPE_STRING, TYPE_STRING};
  static unsigned int length[] = {0, 0, 16, 128};
  int      n, ncol = 4;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  maxres = 512;                       // This is completely arbitrary
  n = ocp->GetMaxValue(SQL_MAX_USER_NAME_LEN);
  length[0] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[1] = (n) ? (n + 1) : 128;

#ifdef DEBTRACE
 htrc("ODBCTables: max=%d len=%d,%d\n",
  maxres, length[0], length[1]);
#endif

  /************************************************************************/
  /*  Allocate the structures used to refer to the result set.            */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_TABLES + 1,
                                        dbtype, buftyp, length);

  cap = AllocCatInfo(g, CAT_TAB, tabpat, qrp);
  cap->Pat = (PUCHAR)tabtyp;

#ifdef DEBTRACE
 htrc("Getting table results ncol=%d\n", cap->Qrp->Nbcol);
#endif

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
    ResetNullValues(cap);

#ifdef DEBTRACE
 htrc("Tables: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);
#endif
  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCTables

/***********************************************************************/
/*  ODBCColumns: constructs the result blocks containing all columns   */
/*  of an ODBC table that will be retrieved by GetData commands.       */
/*  Note: The first two columns (Qualifier, Owner) are ignored.        */
/***********************************************************************/
PQRYRES ODBCColumns(PGLOBAL g, ODBConn *op, char *dsn, char *table,
                                                       char *colpat)
  {
  static int dbtype[] = {DB_CHAR,  DB_CHAR,
                         DB_CHAR,  DB_SHORT, DB_CHAR,
                         DB_INT,  DB_INT,  DB_SHORT,
                         DB_SHORT, DB_SHORT, DB_CHAR};
  static int buftyp[] = {TYPE_STRING, TYPE_STRING,
                         TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                         TYPE_INT,   TYPE_INT,  TYPE_SHORT,
                         TYPE_SHORT,  TYPE_SHORT, TYPE_STRING};
  static unsigned int length[] = {0, 0, 0, 6, 20, 10, 10, 6, 6, 6, 128};
  int      n, ncol = 11;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  n = ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_TABLE);
  maxres = (n) ? (int)n : 250;
  n = ocp->GetMaxValue(SQL_MAX_USER_NAME_LEN);
  length[0] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[1] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
  length[2] = (n) ? (n + 1) : 128;

#ifdef DEBTRACE
 htrc("ODBCColumns: max=%d len=%d,%d,%d\n",
         maxres, length[0], length[1], length[2]);
#endif

  /************************************************************************/
  /*  Allocate the structures used to refer to the result set.            */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_COLUMNS + 1,
                                        dbtype, buftyp, length);

#ifdef DEBTRACE
 htrc("Getting col results ncol=%d\n", qrp->Nbcol);
#endif

  cap = AllocCatInfo(g, CAT_COL, table, qrp);
  cap->Pat = (PUCHAR)colpat;

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
    ResetNullValues(cap);

#ifdef DEBTRACE
 htrc("Columns: NBCOL=%d NBLIN=%d\n", qrp->Nbcol, qrp->Nblin);
#endif
  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCColumns

/**************************************************************************/
/*  PrimaryKeys: constructs the result blocks containing all the          */
/*  ODBC catalog information concerning primary keys.                     */
/**************************************************************************/
PQRYRES ODBCPrimaryKeys(PGLOBAL g, ODBConn *op, char *dsn, char *table)
  {
  static int dbtype[] = {DB_CHAR, DB_CHAR, DB_CHAR, DB_SHORT, DB_CHAR};
  static int buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING,
                         TYPE_SHORT,  TYPE_STRING};
  static unsigned int length[] = {0, 0, 0, 6, 128};
  int      n, ncol = 5;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  n = ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_TABLE);
  maxres = (n) ? (int)n : 250;
  n = ocp->GetMaxValue(SQL_MAX_USER_NAME_LEN);
  length[0] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[1] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
  length[2] = (n) ? (n + 1) : 128;

#ifdef DEBTRACE
 htrc("ODBCPrimaryKeys: max=%d len=%d,%d,%d\n",
         maxres, length[0], length[1], length[2]);
#endif

  /************************************************************************/
  /*  Allocate the structure used to refer to the result set.             */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_PKEY + 1,
                                        dbtype, buftyp, length);

#ifdef DEBTRACE
 htrc("Getting pkey results ncol=%d\n", qrp->Nbcol);
#endif

  cap = AllocCatInfo(g, CAT_KEY, table, qrp);

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
    ResetNullValues(cap);

#ifdef DEBTRACE
 htrc("PrimaryKeys: NBCOL=%d NBLIN=%d\n",
  qrp->Nbcol, qrp->Nblin);
#endif
  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of ODBCPrimaryKeys

/**************************************************************************/
/*  Statistics: constructs the result blocks containing statistics        */
/*  about one or several tables to be retrieved by GetData commands.      */
/**************************************************************************/
PQRYRES ODBCStatistics(PGLOBAL g, ODBConn *op, char *dsn, char *pat,
                                               int un, int acc)
  {
  static int dbtype[] = {DB_CHAR, DB_CHAR,  DB_SHORT, DB_CHAR,
                         DB_CHAR, DB_SHORT, DB_SHORT, DB_CHAR,
                         DB_CHAR, DB_INT,  DB_INT,  DB_CHAR};
  static int buftyp[] = {TYPE_STRING, TYPE_STRING, TYPE_SHORT, TYPE_STRING,
                         TYPE_STRING, TYPE_SHORT,  TYPE_SHORT, TYPE_STRING,
                         TYPE_STRING, TYPE_INT,   TYPE_INT,  TYPE_STRING};
  static unsigned int length[] = {0, 0 ,6 ,0 ,0 ,6 ,6 ,0 ,2 ,10 ,10 ,128};
  int      n, ncol = 12;
  int     maxres;
  PQRYRES  qrp;
  CATPARM *cap;
  ODBConn *ocp = op;

  if (!op) {
    /**********************************************************************/
    /*  Open the connection with the ODBC data source.                    */
    /**********************************************************************/
    ocp = new(g) ODBConn(g, NULL);

    if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
      return NULL;

    } // endif op

  /************************************************************************/
  /*  Do an evaluation of the result size.                                */
  /************************************************************************/
  n = 1 + ocp->GetMaxValue(SQL_MAX_COLUMNS_IN_INDEX);
  maxres = (n) ? (int)n : 32;
  n = ocp->GetMaxValue(SQL_MAX_USER_NAME_LEN);
  length[0] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_TABLE_NAME_LEN);
  length[1] = length[4] = (n) ? (n + 1) : 128;
  n = ocp->GetMaxValue(SQL_MAX_QUALIFIER_NAME_LEN);
  length[3] = (n) ? (n + 1) : length[1];
  n = ocp->GetMaxValue(SQL_MAX_COLUMN_NAME_LEN);
  length[7] = (n) ? (n + 1) : 128;

#ifdef DEBTRACE
 htrc("SemStatistics: max=%d pat=%s\n", maxres, SVP(pat));
#endif

  /************************************************************************/
  /*  Allocate the structure used to refer to the result set.             */
  /************************************************************************/
  qrp = PlgAllocResult(g, ncol, maxres, IDS_STAT + 1,
                                        dbtype, buftyp, length);

#ifdef DEBTRACE
 htrc("Getting stat results ncol=%d\n", qrp->Nbcol);
#endif

  cap = AllocCatInfo(g, CAT_STAT, pat, qrp);
  cap->Unique = (un < 0) ? SQL_INDEX_UNIQUE : (UWORD)un;
  cap->Accuracy = (acc < 0) ? SQL_QUICK : (UWORD)acc;

  /************************************************************************/
  /*  Now get the results into blocks.                                    */
  /************************************************************************/
  if ((n = ocp->GetCatInfo(cap)) >= 0) {
    qrp->Nblin = n;
    ResetNullValues(cap);

#ifdef DEBTRACE
 htrc("Statistics: NBCOL=%d NBLIN=%d\n",
  qrp->Nbcol, qrp->Nblin);
#endif
  } else
    qrp = NULL;

  /************************************************************************/
  /*  Close any local connection.                                         */
  /************************************************************************/
  if (!op)
    ocp->Close();

  /************************************************************************/
  /*  Return the result pointer for use by GetData routines.              */
  /************************************************************************/
  return qrp;
  } // end of Statistics

/***********************************************************************/
/*  GetColumnInfo: used when defining a ODBC table. The issue is that  */
/*  some ODBC drivers give key information by SQLPrimaryKeys while     */
/*  others do not implement it but give info using SQLStatistics.      */
/***********************************************************************/
PQRYRES GetColumnInfo(PGLOBAL g, char*& dsn,
                                 char *table, int ver, PVBLK& vbp)
  {
  PCOLRES  crp;
  PQRYRES  qrpc, qrp;
  PVBLK    vbp2;
  ODBConn *ocp = new(g) ODBConn(g, NULL);

  /**********************************************************************/
  /*  Open the connection with the ODBC data source.                    */
  /**********************************************************************/
  if (ocp->Open(dsn, 2) < 1)        // 2 is openReadOnly
    return NULL;
  else if (ver > 0)
    ocp->m_Catver = ver;

  /**********************************************************************/
  /*  Get the information about the ODBC table columns.                 */
  /**********************************************************************/
  if ((qrpc = ODBCColumns(g, ocp, dsn, table, NULL)))
    dsn = ocp->GetConnect();        // Complete connect string
  else
    return NULL;

  if ((qrp = ODBCPrimaryKeys(g, ocp, dsn, table))) {
    // Oracle, ...
    if (qrp->Nblin) {
      crp = qrp->Colresp->Next->Next;
      vbp = crp->Kdata;
      vbp->ReAlloc(vbp->GetValPointer(), qrp->Nblin);
      } // endif Nblin

  } else if ((qrp = ODBCStatistics(g, ocp, dsn, table, -1, -1))) {
    // Case of Microsoft Jet Engine
    if (qrp->Nblin) {
      int     i, n = 0;
      PCOLRES crp2;

      crp = qrp->Colresp->Next->Next->Next->Next;
      crp2 = crp->Next->Next->Next;

      // This test may have to be modified for other ODBC drivers
      for (i = 0; i < qrp->Nblin; i++)
        if (!strcmp(crp->Kdata->GetCharValue(i), "PrimaryKey"))
          n++;

      if (n) {
        vbp2 = crp2->Kdata;
        vbp = AllocValBlock(g, NULL, vbp2->GetType(), n,
                                     vbp2->GetVlen(), 0, false, false);

        for (i = 0, n = 0; i < qrp->Nblin; i++)
          if (!strcmp(crp->Kdata->GetCharValue(i), "PrimaryKey"))
            vbp->SetValue(vbp2, n++, i);

        } // endif n

      } // endif Nblin

  } // endif qrp

  /************************************************************************/
  /*  Close the local connection.                                         */
  /************************************************************************/
  ocp->Close();

  return qrpc;
  } // end of GetColumnInfo


/***********************************************************************/
/*  Implementation of DBX class.                                       */
/***********************************************************************/
DBX::DBX(RETCODE rc)
  {
  m_RC = rc;

  for (int i = 0; i < MAX_NUM_OF_MSG; i++)
    m_ErrMsg[i] = NULL;

  } // end of DBX constructor

/***********************************************************************/
/*  This function is called by ThrowDBX.                               */
/***********************************************************************/
void DBX::BuildErrorMessage(ODBConn* pdb, HSTMT hstmt)
  {
  if (pdb) {
    SWORD   len;
    RETCODE rc;
    UCHAR   msg[SQL_MAX_MESSAGE_LENGTH + 1];
    UCHAR   state[SQL_SQLSTATE_SIZE + 1];
    SDWORD  native;
    PGLOBAL g = pdb->m_G;

    rc = SQLError(pdb->m_henv, pdb->m_hdbc, hstmt, state,
                  &native, msg, SQL_MAX_MESSAGE_LENGTH - 1, &len);

    if (rc != SQL_INVALID_HANDLE)
    // Skip non-errors
      for (int i = 0; i < MAX_NUM_OF_MSG
              && (rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO)
              && strcmp((char*)state, "00000"); i++) {
        m_ErrMsg[i] = (PSZ)PlugSubAlloc(g, NULL, strlen((char*)msg) + 1);
        strcpy(m_ErrMsg[i], (char*)msg);

#ifdef DEBTRACE
 htrc("%s, Native=%d\n", msg, native);
#endif

        rc = SQLError(pdb->m_henv, pdb->m_hdbc, hstmt, state,
                      &native, msg, SQL_MAX_MESSAGE_LENGTH - 1, &len);
        } // endfor i

    else
      m_ErrMsg[0] = MSG(BAD_HANDLE_VAL);

  } else
    m_ErrMsg[0] = "No connexion address provided";

  } // end of BuildErrorMessage

/***********************************************************************/
/*  ODBConn construction/destruction.                                  */
/***********************************************************************/
ODBConn::ODBConn(PGLOBAL g, TDBODBC *tdbp)
  {
  m_G = g;
  m_Tdb = tdbp;
  m_hdbc = SQL_NULL_HDBC;
//m_Recset = NULL
  m_hstmt = SQL_NULL_HSTMT;
  m_LoginTimeout = DEFAULT_LOGIN_TIMEOUT;
  m_QueryTimeout = DEFAULT_QUERY_TIMEOUT;
  m_UpdateOptions = 0;
  m_RowsetSize = (DWORD)((tdbp) ? tdbp->Rows : 10);
  m_Catver = (tdbp) ? tdbp->Catver : 0;
  m_Connect = NULL;
  m_Updatable = true;
//m_Transactions = false;
  m_IDQuoteChar = '\'';
//*m_ErrMsg = '\0';
  } // end of ODBConn

//ODBConn::~ODBConn()
//  {
//if (Connected())
//  EndCom();

//  } // end of ~ODBConn

/***********************************************************************/
/*  Screen for errors.                                                 */
/***********************************************************************/
bool ODBConn::Check(RETCODE rc)
  {
  switch (rc) {
    case SQL_SUCCESS_WITH_INFO:
      if (m_G->Trace) {
        DBX x(rc);

        x.BuildErrorMessage(this, m_hstmt);
        htrc("ODBC Success With Info, hstmt=%p %s\n",
          m_hstmt, x.GetErrorMessage(0));
        } // endif Trace

      // Fall through
    case SQL_SUCCESS:
    case SQL_NO_DATA_FOUND:
      return true;
    } // endswitch rc

  return false;
  } // end of Check

/***********************************************************************/
/*  DB exception throw routines.                                       */
/***********************************************************************/
void ODBConn::ThrowDBX(RETCODE rc, HSTMT hstmt)
  {
  DBX* xp = new(m_G) DBX(rc);

  xp->BuildErrorMessage(this, hstmt);
  throw xp;
  } // end of ThrowDBX

void ODBConn::ThrowDBX(PSZ msg)
  {
  DBX* xp = new(m_G) DBX(0);

  xp->m_ErrMsg[0] = msg;
  throw xp;
  } // end of ThrowDBX

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
PSZ ODBConn::GetStringInfo(ushort infotype)
  {
//ASSERT(m_hdbc != SQL_NULL_HDBC);
  char   *p, buffer[MAX_STRING_INFO];
  SWORD   result;
  RETCODE rc;

  rc = SQLGetInfo(m_hdbc, infotype, buffer, sizeof(buffer), &result);

  if (!Check(rc))
    ThrowDBX(rc);  // Temporary
//  *buffer = '\0';

  p = (char *)PlugSubAlloc(m_G, NULL, strlen(buffer) + 1);
  strcpy(p, buffer);
  return p;
  } // end of GetStringInfo

/***********************************************************************/
/*  Utility routine.                                                   */
/***********************************************************************/
int ODBConn::GetMaxValue(ushort infotype)
  {
//ASSERT(m_hdbc != SQL_NULL_HDBC);
  ushort  maxval;
  RETCODE rc;

  rc = SQLGetInfo(m_hdbc, infotype, &maxval, 0, NULL);

  if (!Check(rc))
    maxval = 0;

  return (int)maxval;
  } // end of GetMaxValue

/***********************************************************************/
/*  Utility routines.                                                  */
/***********************************************************************/
void ODBConn::OnSetOptions(HSTMT hstmt)
  {
  RETCODE rc;
  ASSERT(m_hdbc != SQL_NULL_HDBC);

  if ((signed)m_QueryTimeout != -1) {
    // Attempt to set query timeout.  Ignore failure
    rc = SQLSetStmtOption(hstmt, SQL_QUERY_TIMEOUT, m_QueryTimeout);

    if (!Check(rc))
      // don't attempt it again
      m_QueryTimeout = (DWORD)-1;

    } // endif m_QueryTimeout

  if (m_RowsetSize > 0) {
    // Attempt to set rowset size.
    // In case of failure reset it to 0 to use Fetch.
    rc = SQLSetStmtOption(hstmt, SQL_ROWSET_SIZE, m_RowsetSize);

    if (!Check(rc))
      // don't attempt it again
      m_RowsetSize = 0;

    } // endif m_RowsetSize

  } // end of OnSetOptions

/***********************************************************************/
/*  Open: connect to a data source.                                    */
/***********************************************************************/
int ODBConn::Open(PSZ ConnectString, DWORD options)
  {
  PGLOBAL& g = m_G;
//ASSERT_VALID(this);
//ASSERT(ConnectString == NULL || AfxIsValidString(ConnectString));
  ASSERT(!(options & noOdbcDialog && options & forceOdbcDialog));

  m_Updatable = !(options & openReadOnly);
  m_Connect = ConnectString;

  // Allocate the HDBC and make connection
  try {
    PSZ ver;

    AllocConnect(options);
    ver = GetStringInfo(SQL_ODBC_VER);

    if (Connect(options)) {
      strcpy(g->Message, MSG(CONNECT_CANCEL));
      return 0;
      } // endif

    ver = GetStringInfo(SQL_DRIVER_ODBC_VER);
  } catch(DBX *xp) {
//    strcpy(g->Message, xp->m_ErrMsg[0]);
    strcpy(g->Message, xp->GetErrorMessage(0));
    Free();
    return -1;
  } // end try-catch

  // Verify support for required functionality and cache info
  VerifyConnect();
  GetConnectInfo();
  return 1;
  } // end of Open

/***********************************************************************/
/*  Allocate an henv (first time called) and hdbc.                     */
/***********************************************************************/
void ODBConn::AllocConnect(DWORD Options)
  {
  if (m_hdbc != SQL_NULL_HDBC)
    return;

  RETCODE rc;
//AfxLockGlobals(CRIT_ODBC);

  // Need to allocate an environment for first connection
  if (m_henv == SQL_NULL_HENV) {
    ASSERT(m_nAlloc == 0);

    rc = SQLAllocEnv(&m_henv);

    if (!Check(rc)) {
//    AfxUnlockGlobals(CRIT_ODBC);
      ThrowDBX(rc);  // Fatal
      } // endif

    } // endif m_henv

  // Do the real thing, allocating connection data
  rc = SQLAllocConnect(m_henv, &m_hdbc);

  if (!Check(rc)) {
//  AfxUnlockGlobals(CRIT_ODBC);
    ThrowDBX(rc);  // Fatal
    } // endif

  m_nAlloc++;                          // allocated at last
//AfxUnlockGlobals(CRIT_ODBC);

#if defined(_DEBUG)
  if (Options & traceSQL) {
    SQLSetConnectOption(m_hdbc, SQL_OPT_TRACEFILE, (DWORD)"xodbc.out");
    SQLSetConnectOption(m_hdbc, SQL_OPT_TRACE, 1);
    } // endif
#endif // _DEBUG

  rc = SQLSetConnectOption(m_hdbc, SQL_LOGIN_TIMEOUT, m_LoginTimeout);

#ifdef DEBTRACE
 if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
  htrc("Warning: Failure setting login timeout\n");
#endif

  if (!m_Updatable) {
    rc = SQLSetConnectOption(m_hdbc, SQL_ACCESS_MODE,
                                     SQL_MODE_READ_ONLY);
#ifdef DEBTRACE
 if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO)
  htrc("Warning: Failure setting read only access mode\n");
#endif
    } // endif

  // Turn on cursor lib support
  if (Options & useCursorLib)
    rc = SQLSetConnectOption(m_hdbc, SQL_ODBC_CURSORS, SQL_CUR_USE_ODBC);

  return;
  } // end of AllocConnect

/***********************************************************************/
/*  Connect to data source using SQLDriverConnect.                     */
/***********************************************************************/
bool ODBConn::Connect(DWORD Options)
  {
  RETCODE rc;
  SWORD   nResult;
  PUCHAR  ConnOut = (PUCHAR)PlugSubAlloc(m_G, NULL, MAX_CONNECT_LEN);
  UWORD   wConnectOption = SQL_DRIVER_COMPLETE;
#if defined(WIN32)
  HWND    hWndTop = GetForegroundWindow();
  HWND    hWnd = GetParent(hWndTop);

  if (hWnd == NULL)
    hWnd = GetDesktopWindow();
#else   // !WIN32
  HWND    hWnd = NULL;
#endif  // !WIN32
  PGLOBAL& g = m_G;
  PDBUSER dup = PlgGetUser(g);

  if (Options & noOdbcDialog || dup->Remote)
    wConnectOption = SQL_DRIVER_NOPROMPT;
  else if (Options & forceOdbcDialog)
    wConnectOption = SQL_DRIVER_PROMPT;

  rc = SQLDriverConnect(m_hdbc, hWnd, (PUCHAR)m_Connect,
                        SQL_NTS, ConnOut, MAX_CONNECT_LEN,
                        &nResult, wConnectOption);

#if defined(WIN32)
  if (hWndTop)
    EnableWindow(hWndTop, true);
#endif   // WIN32

  // If user hit 'Cancel'
  if (rc == SQL_NO_DATA_FOUND) {
    Free();
    return true;
    } // endif rc

  if (!Check(rc)) {
#ifdef DEBTRACE
 if (!hWnd == NULL)
  htrc("Error: No default window for SQLDriverConnect\n");
#endif
    ThrowDBX(rc);
    } // endif Check

  // Save connect string returned from ODBC
  m_Connect = (PSZ)ConnOut;

  // All done
  return false;
  } // end of Connect

void ODBConn::VerifyConnect()
  {
#if defined(NEWMSG) || defined(XMSG)
  PGLOBAL& g = m_G;
#endif   // NEWMSG	||				 XMSG
  RETCODE  rc;
  SWORD    result;
  SWORD    conformance;

  rc = SQLGetInfo(m_hdbc, SQL_ODBC_API_CONFORMANCE,
                  &conformance, sizeof(conformance), &result);

  if (!Check(rc))
    ThrowDBX(rc);

  if (conformance < SQL_OAC_LEVEL1)
    ThrowDBX(MSG(API_CONF_ERROR));

  rc = SQLGetInfo(m_hdbc, SQL_ODBC_SQL_CONFORMANCE,
                  &conformance, sizeof(conformance), &result);

  if (!Check(rc))
    ThrowDBX(rc);

  if (conformance < SQL_OSC_MINIMUM)
    ThrowDBX(MSG(SQL_CONF_ERROR));

  } // end of VerifyConnect

void ODBConn::GetConnectInfo()
  {
  RETCODE rc;
  SWORD   nResult;
#if 0                   // Update not implemented yet
  UDWORD  DrvPosOp;

  // Reset the database update options
  m_UpdateOptions = 0;

  // Check for SQLSetPos support
  rc = SQLGetInfo(m_hdbc, SQL_POS_OPERATIONS,
                  &DrvPosOp, sizeof(DrvPosOp), &nResult);

  if (Check(rc) &&
      (DrvPosOp & SQL_POS_UPDATE) &&
      (DrvPosOp & SQL_POS_DELETE) &&
      (DrvPosOp & SQL_POS_ADD))
     m_UpdateOptions = SQL_SETPOSUPDATES;

  // Check for positioned update SQL support
  UDWORD PosStatements;

  rc = SQLGetInfo(m_hdbc, SQL_POSITIONED_STATEMENTS,
                        &PosStatements, sizeof(PosStatements),
                        &nResult);

  if (Check(rc) &&
      (PosStatements & SQL_PS_POSITIONED_DELETE) &&
      (PosStatements & SQL_PS_POSITIONED_UPDATE))
    m_UpdateOptions |= SQL_POSITIONEDSQL;

  if (m_Updatable) {
    // Make sure data source is Updatable
    char ReadOnly[10];

    rc = SQLGetInfo(m_hdbc, SQL_DATA_SOURCE_READ_ONLY,
                    ReadOnly, sizeof(ReadOnly), &nResult);

    if (Check(rc) && nResult == 1)
      m_Updatable = !!strcmp(ReadOnly, "Y");
    else
      m_Updatable = false;

#ifdef DEBTRACE
 htrc("Warning: data source is readonly\n");
#endif
  } else // Make data source is !Updatable
    rc = SQLSetConnectOption(m_hdbc, SQL_ACCESS_MODE,
                                     SQL_MODE_READ_ONLY);
#endif   // 0

  // Cache the quote char to use when constructing SQL
  char QuoteChar[2];

  rc = SQLGetInfo(m_hdbc, SQL_IDENTIFIER_QUOTE_CHAR,
                  QuoteChar, sizeof(QuoteChar), &nResult);

  if (Check(rc) && nResult == 1)
    m_IDQuoteChar = QuoteChar[0];
  else
    m_IDQuoteChar = ' ';

#ifdef DEBTRACE
 htrc("DBMS: %s, Version: %s",
  GetStringInfo(SQL_DBMS_NAME), GetStringInfo(SQL_DBMS_VER));
#endif // DEBTRACE
  } // end of GetConnectInfo

/***********************************************************************/
/*  Allocate record set and execute an SQL query.                      */
/***********************************************************************/
int ODBConn::ExecDirectSQL(char *sql, ODBCCOL *tocols)
  {
  PGLOBAL& g = m_G;
  void    *buffer;
  bool     b;
  UWORD    n;
  SWORD    ncol, len, tp;
  SQLLEN   afrw;
  ODBCCOL *colp;
  RETCODE  rc;
  HSTMT    hstmt;

//m_Recset = new(m_G) RECSET(this);
//ASSERT(m_Recset);

  try {
    b = false;

    if (m_hstmt) {
      RETCODE  rc;

//    All this did not seems to make sense and was been commented out
//    if (IsOpen())
//      Close(SQL_CLOSE);

      rc = SQLFreeStmt(m_hstmt, SQL_CLOSE);
      hstmt = m_hstmt;
      m_hstmt = NULL;
      ThrowDBX(MSG(SEQUENCE_ERROR));
    } else {
      rc = SQLAllocStmt(m_hdbc, &hstmt);

      if (!Check(rc))
        ThrowDBX(SQL_INVALID_HANDLE);

    } // endif hstmt

    OnSetOptions(hstmt);
    b = true;

    if (g->Trace) {
      htrc("ExecDirect hstmt=%p %.64s\n", hstmt, sql);
      fflush(debug);
      } // endif Trace

    do {
      rc = SQLExecDirect(hstmt, (PUCHAR)sql, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, hstmt);

    do {
      rc = SQLNumResultCols(hstmt, &ncol);
      } while (rc == SQL_STILL_EXECUTING);

    if (ncol == 0) {
      // Update or Delete statement
      rc = SQLRowCount(hstmt, &afrw);

      if (!Check(rc))
        ThrowDBX(rc, hstmt);

      return afrw;
      } // endif ncol

    for (n = 0, colp = tocols; colp; colp = (PODBCCOL)colp->GetNext())
      if (!colp->IsSpecial())
      n++;

    // n can be 0 for query such as Select count(*) from table
    if (n && n != (UWORD)ncol)
      ThrowDBX(MSG(COL_NUM_MISM));

    // Now bind the column buffers
    for (n = 1, colp = tocols; colp; colp = (PODBCCOL)colp->GetNext())
      if (!colp->IsSpecial()) {
        buffer = colp->GetBuffer(m_RowsetSize);
        len = colp->GetBuflen();
        tp = GetSQLCType(colp->GetResultType());

        if (tp == SQL_TYPE_NULL) {
          sprintf(m_G->Message, MSG(INV_COLUMN_TYPE),
                  colp->GetResultType(), SVP(colp->GetName()));
          ThrowDBX(m_G->Message);
          } // endif tp

        if (g->Trace) {
          htrc("Binding col=%u type=%d buf=%p len=%d slen=%p\n",
                  n, tp, buffer, len, colp->GetStrLen());
          fflush(debug);
          } // endif Trace

        rc = SQLBindCol(hstmt, n, tp, buffer, len, colp->GetStrLen());

        if (!Check(rc))
          ThrowDBX(rc, hstmt);

        n++;
        } // endif pcol

  } catch(DBX *x) {
#ifdef DEBTRACE
 for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
  htrc(x->m_ErrMsg[i]);
#endif
    strcpy(m_G->Message, x->GetErrorMessage(0));

    if (b)
      SQLCancel(hstmt);

    rc = SQLFreeStmt(hstmt, SQL_DROP);
    m_hstmt = NULL;
    return -1;
  } // end try/catch

  m_hstmt = hstmt;
  return (int)m_RowsetSize;   // May have been reset in OnSetOptions
  } // end of ExecDirectSQL

/***********************************************************************/
/*  Get the number of lines of the result set.                         */
/***********************************************************************/
int ODBConn::GetResultSize(char *sql, ODBCCOL *colp)
  {
  int    n = 0;
  RETCODE rc;

  if (ExecDirectSQL(sql, colp) < 0)
    return -1;

  try {
    for (n = 0; ; n++) {
      do {
        rc = SQLFetch(m_hstmt);
        } while (rc == SQL_STILL_EXECUTING);

      if (!Check(rc))
        ThrowDBX(rc, m_hstmt);

      if (rc == SQL_NO_DATA_FOUND)
        break;

      } // endfor n

  } catch(DBX *x) {
//    strcpy(m_G->Message, x->m_ErrMsg[0]);
    strcpy(m_G->Message, x->GetErrorMessage(0));
#ifdef DEBTRACE
 for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
  htrc(x->m_ErrMsg[i]);
#endif
    SQLCancel(m_hstmt);
    n = -2;
  } // end try/catch

  rc = SQLFreeStmt(m_hstmt, SQL_DROP);
  m_hstmt = NULL;

  if (n != 1)
    return -3;
  else
    return colp->GetIntValue();

  } // end of GetResultSize

/***********************************************************************/
/*  Fetch next row.                                                    */
/***********************************************************************/
int ODBConn::Fetch()
  {
  ASSERT(m_hstmt);
  int      irc;
  SQLULEN  crow;
  RETCODE  rc;
  PGLOBAL& g = m_G;

  try {
//  do {
    if (m_RowsetSize) {
      rc = SQLExtendedFetch(m_hstmt, SQL_FETCH_NEXT, 1, &crow, NULL);
    } else {
      rc = SQLFetch(m_hstmt);
      crow = 1;
    } // endif m_RowsetSize
//    } while (rc == SQL_STILL_EXECUTING);

    if (g->Trace)
      htrc("Fetch: hstmt=%p RowseSize=%d rc=%d\n",
                     m_hstmt, m_RowsetSize, rc);

    if (!Check(rc))
      ThrowDBX(rc, m_hstmt);

    irc = (rc == SQL_NO_DATA_FOUND) ? 0 : (int)crow;
  } catch(DBX *x) {
    if (g->Trace)
      for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
        htrc(x->m_ErrMsg[i]);

    strcpy(g->Message, x->GetErrorMessage(0));
    irc = -1;
  } // end try/catch

  return irc;
  } // end of Fetch

/***********************************************************************/
/*  Prepare an SQL statement for insert.                               */
/***********************************************************************/
int ODBConn::PrepareSQL(char *sql)
  {
  PGLOBAL& g = m_G;
  bool     b;
  SWORD    nparm;
  RETCODE  rc;
  HSTMT    hstmt;

  try {
    b = false;

    if (m_hstmt) {
      RETCODE rc = SQLFreeStmt(m_hstmt, SQL_CLOSE);

      hstmt = m_hstmt;
      m_hstmt = NULL;
      ThrowDBX(MSG(SEQUENCE_ERROR));
    } else {
      rc = SQLAllocStmt(m_hdbc, &hstmt);

      if (!Check(rc))
        ThrowDBX(SQL_INVALID_HANDLE);

    } // endif hstmt

    OnSetOptions(hstmt);
    b = true;

    if (g->Trace) {
      htrc("Prepare hstmt=%p %.64s\n", hstmt, sql);
      fflush(debug);
      } // endif Trace

    do {
      rc = SQLPrepare(hstmt, (PUCHAR)sql, SQL_NTS);
      } while (rc == SQL_STILL_EXECUTING);

    if (!Check(rc))
      ThrowDBX(rc, hstmt);

    do {
      rc = SQLNumParams(hstmt, &nparm);
      } while (rc == SQL_STILL_EXECUTING);

  } catch(DBX *x) {
#ifdef DEBTRACE
 for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
  htrc(x->m_ErrMsg[i]);
#endif
    strcpy(m_G->Message, x->GetErrorMessage(0));

    if (b)
      SQLCancel(hstmt);

    rc = SQLFreeStmt(hstmt, SQL_DROP);
    m_hstmt = NULL;
    return -1;
  } // end try/catch

  m_hstmt = hstmt;
  return (int)nparm;
  } // end of PrepareSQL

/***********************************************************************/
/*  Bind a parameter for inserting.                                    */
/***********************************************************************/
bool ODBConn::ExecuteSQL(void)
  {
  RETCODE rc;

  try {
    rc = SQLExecute(m_hstmt);

    if (!Check(rc))
      ThrowDBX(rc, m_hstmt);

  } catch(DBX *x) {
    strcpy(m_G->Message, x->GetErrorMessage(0));
    SQLCancel(m_hstmt);
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;
    return true;
  } // end try/catch

  return false;
  } // end of ExecuteSQL

/***********************************************************************/
/*  Bind a parameter for inserting.                                    */
/***********************************************************************/
bool ODBConn::BindParam(ODBCCOL *colp)
  {
  void   *buf;
  UWORD   n = colp->GetRank();
  SWORD   ct, sqlt;
  UDWORD  len;
  SQLLEN *strlen = colp->GetStrLen();
  RETCODE rc;

#if 0
  try {
    SWORD   dec, nul;
    rc = SQLDescribeParam(m_hstmt, n, &sqlt, &len, &dec, &nul);

    if (!Check(rc))
      ThrowDBX(rc, m_hstmt);

  } catch(DBX *x) {
    strcpy(m_G->Message, x->GetErrorMessage(0));
  } // end try/catch
#endif // 0

  buf = colp->GetBuffer(0);
//  len = colp->GetBuflen();
  len = IsTypeNum(colp->GetResultType()) ? 0 : colp->GetBuflen();
  ct = GetSQLCType(colp->GetResultType());
  sqlt = GetSQLType(colp->GetResultType());
  *strlen = IsTypeNum(colp->GetResultType()) ? 0 : SQL_NTS;

  try {
    rc = SQLBindParameter(m_hstmt, n, SQL_PARAM_INPUT, ct, sqlt,
                          len, 0, buf, 0, strlen);

    if (!Check(rc))
      ThrowDBX(rc, m_hstmt);

  } catch(DBX *x) {
    strcpy(m_G->Message, x->GetErrorMessage(0));
    SQLCancel(m_hstmt);
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;
    return true;
  } // end try/catch

  return false;
  } // end of BindParam

/***********************************************************************/
/*  Allocate recset and call SQLTables, SQLColumns or SQLPrimaryKeys.  */
/***********************************************************************/
int ODBConn::GetCatInfo(CATPARM *cap)
  {
#if defined(NEWMSG) || defined(XMSG)
  PGLOBAL& g = m_G;
#endif   // NEWMSG	||				 XMSG
  void    *buffer;
  int      i, irc;
  bool     b;
  UWORD    n;
  SWORD    ncol, len, tp;
  SQLULEN  crow;
  PCOLRES  crp;
  RETCODE  rc;
  HSTMT    hstmt = NULL;
  SQLLEN  *vl, *vlen;
  PVAL    *pval = NULL;

  try {
    b = false;

    if (!m_hstmt) {
      rc = SQLAllocStmt(m_hdbc, &hstmt);

      if (!Check(rc))
        ThrowDBX(SQL_INVALID_HANDLE);

    } else
      ThrowDBX(MSG(SEQUENCE_ERROR));

    b = true;

    if ((m_RowsetSize = cap->Qrp->Maxres) > 0) {
      if (m_Catver) {
        // Attempt to set rowset size.
        // In case of failure reset it to 0 to use Fetch.
        if (m_Catver == 3)          // ODBC Ver 3
          rc = SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_ARRAY_SIZE,
                                    (SQLPOINTER)m_RowsetSize, 0);
        else
          rc = SQLSetStmtOption(hstmt, SQL_ROWSET_SIZE, m_RowsetSize);

        if (!Check(rc))
          m_RowsetSize = 1;        // don't attempt it again
//        ThrowDBX(rc, hstmt);      // Temporary

        if (m_Catver == 3) {        // ODBC Ver 3
          rc = SQLSetStmtAttr(hstmt, SQL_ATTR_ROW_STATUS_PTR, cap->Status, 0);
          rc = SQLSetStmtAttr(hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &crow, 0);
          } // endif m_Catver

      } else  // ORABUG
        m_RowsetSize = 1;

    } else
      ThrowDBX("0-sized result");

    // Now do call the proper ODBC API
    switch (cap->Id) {
      case CAT_TAB:
//      rc = SQLSetStmtAttr(hstmt, SQL_ATTR_METADATA_ID,
//                                (SQLPOINTER)false, 0);
        rc = SQLTables(hstmt, NULL, 0, NULL, 0, cap->Tab, SQL_NTS,
                                                cap->Pat, SQL_NTS);
        break;
      case CAT_COL:
//      rc = SQLSetStmtAttr(hstmt, SQL_ATTR_METADATA_ID,
//                                (SQLPOINTER)true, 0);
        rc = SQLColumns(hstmt, NULL, 0, NULL, 0, cap->Tab, SQL_NTS,
                                                 cap->Pat, SQL_NTS);
        break;
      case CAT_KEY:
        rc = SQLPrimaryKeys(hstmt, NULL, 0, NULL, 0, cap->Tab, SQL_NTS);
        break;
      case CAT_STAT:
        rc = SQLStatistics(hstmt, NULL, 0, NULL, 0, cap->Tab, SQL_NTS,
                                  cap->Unique, cap->Accuracy);
        break;
      case CAT_SPC:
        ThrowDBX("SQLSpecialColumns not available yet");
      } // endswitch infotype

    if (!Check(rc))
      ThrowDBX(rc, hstmt);

    rc = SQLNumResultCols(hstmt, &ncol);

    // n + 1 because we ignore the first column
    if ((n = (UWORD)cap->Qrp->Nbcol) + 1 > (UWORD)ncol)
      ThrowDBX(MSG(COL_NUM_MISM));

    if (m_RowsetSize == 1 && cap->Qrp->Maxres > 1) {
      pval = (PVAL *)PlugSubAlloc(m_G, NULL, n * sizeof(PVAL));
      vlen = (SQLLEN *)PlugSubAlloc(m_G, NULL, n * sizeof(SDWORD *));
      } // endif

    // Now bind the column buffers
    for (n = 0, crp = cap->Qrp->Colresp; crp; crp = crp->Next) {
      if (pval) {
        pval[n] = AllocateValue(m_G, crp->Kdata->GetType(),
                                     crp->Kdata->GetVlen(), 0);
        buffer = pval[n]->GetTo_Val();
        vl = vlen + n;
      } else {
        buffer = crp->Kdata->GetValPointer();
        vl = cap->Vlen[n];
      } // endif pval

      len = GetTypeSize(crp->Type, crp->Clen);
      tp = GetSQLCType(crp->Type);

      if (tp == SQL_TYPE_NULL) {
        sprintf(m_G->Message, MSG(INV_COLUMN_TYPE), crp->Type, crp->Name);
        ThrowDBX(m_G->Message);
        } // endif tp

      // n + 2 because column numbers begin with 1 and because
      // we ignore the first column
      rc = SQLBindCol(hstmt, n + 2, tp, buffer, len, vl);

      if (!Check(rc))
        ThrowDBX(rc, hstmt);

      n++;
      } // endfor crp

    // Now fetch the result
    if (m_Catver != 3) {
      if (m_RowsetSize > 1) {
        rc = SQLExtendedFetch(hstmt, SQL_FETCH_NEXT, 1, &crow, cap->Status);
      } else if (pval) {
        for (n = 0; n < cap->Qrp->Maxres; n++) {
          if ((rc = SQLFetch(hstmt)) != SQL_SUCCESS)
            break;

          for (i = 0, crp = cap->Qrp->Colresp; crp; i++, crp = crp->Next) {
            crp->Kdata->SetValue(pval[i], n);
            cap->Vlen[i][n] = vlen[i];
            } // endfor crp

          } // endfor n

        if ((crow = n) && rc == SQL_NO_DATA)
          rc = SQL_SUCCESS;

      } else {
        rc = SQLFetch(hstmt);
        crow = 1;
      } // endif's

    } else    // ODBC Ver 3
      rc = SQLFetch(hstmt);

//  if (!Check(rc))
    if (rc == SQL_NO_DATA_FOUND) {
      if (cap->Pat)
        sprintf(m_G->Message, MSG(NO_TABCOL_DATA), cap->Tab, cap->Pat);
      else
        sprintf(m_G->Message, MSG(NO_TAB_DATA), cap->Tab);

      ThrowDBX(m_G->Message);
    } else if (rc != SQL_SUCCESS)
      ThrowDBX(rc, hstmt);

    irc = (int)crow;
  } catch(DBX *x) {
#ifdef DEBTRACE
 for (int i = 0; i < MAX_NUM_OF_MSG && x->m_ErrMsg[i]; i++)
  htrc(x->m_ErrMsg[i]);
#endif
    strcpy(m_G->Message, x->GetErrorMessage(0));
    irc = -1;
  } // end try/catch

  if (b)
    SQLCancel(hstmt);

  // All this (hstmt vs> m_hstmt) to be revisited
  if (hstmt)
    rc = SQLFreeStmt(hstmt, SQL_DROP);

  return irc;
  } // end of GetCatInfo

/***********************************************************************/
/*  Disconnect connection                                              */
/***********************************************************************/
void ODBConn::Close()
  {
  RETCODE rc;

#if 0
  // Close any open recordsets
  AfxLockGlobals(CRIT_ODBC);
  TRY
  {
    while (!m_listRecordsets.IsEmpty())
    {
      CRecordset* pSet = (CRecordset*)m_listRecordsets.GetHead();
      pSet->Close();  // will implicitly remove from list
      pSet->m_pDatabase = NULL;
    }
  }
  CATCH_ALL(e)
  {
    AfxUnlockGlobals(CRIT_ODBC);
    THROW_LAST();
  }
  END_CATCH_ALL
  AfxUnlockGlobals(CRIT_ODBC);
#endif // 0

  if (m_hstmt) {
    // Is required for multiple tables
    rc = SQLFreeStmt(m_hstmt, SQL_DROP);
    m_hstmt = NULL;
    } // endif m_hstmt

  if (m_hdbc != SQL_NULL_HDBC) {
    rc = SQLDisconnect(m_hdbc);
    rc = SQLFreeConnect(m_hdbc);
    m_hdbc = SQL_NULL_HDBC;

//  AfxLockGlobals(CRIT_ODBC);
    ASSERT(m_nAlloc != 0);
    m_nAlloc--;
//  AfxUnlockGlobals(CRIT_ODBC);
    } // endif m_hdbc

  } // end of Close

// Silently disconnect and free all ODBC resources.
// Don't throw any exceptions
void ODBConn::Free()
  {
  // Trap failures upon close
  try {
    Close();
  } catch(DBX*) {
    // Nothing we can do
#ifdef DEBTRACE
 htrc("Error: exception by Close ignored in Free\n");
#endif
//  DELETE_EXCEPTION(x);
  } // endcatch

  // free henv if refcount goes to 0
//AfxLockGlobals(CRIT_ODBC);
  if (m_henv != SQL_NULL_HENV) {
    ASSERT(m_nAlloc >= 0);

    if (m_nAlloc == 0) {
      // free last connection - release HENV
#ifdef DEBTRACE
 RETCODE rc = SQLFreeEnv(m_henv);
 if (rc != SQL_SUCCESS) // Nothing we can do
  htrc("Error: SQLFreeEnv failure ignored in Free\n");
#else
      SQLFreeEnv(m_henv);
#endif
      m_henv = SQL_NULL_HENV;
      } // endif m_nAlloc
  }
//AfxUnlockGlobals(CRIT_ODBC);
  } // end of Free

#if 0
//////////////////////////////////////////////////////////////////////////////
// CRecordset helpers

//id AFXAPI AfxSetCurrentRecord(int* plCurrentRecord, int nRows, RETCODE nRetCode);
//id AFXAPI AfxSetRecordCount(int* plRecordCount, int lCurrentRecord,
//bool bEOFSeen, RETCODE nRetCode);

/***********************************************************************/
/*  RECSET class implementation                                        */
/***********************************************************************/
RECSET::RECSET(ODBConn *dbcp)
  {
  m_pDB = dbcp;
  m_hstmt = SQL_NULL_HSTMT;
  m_OpenType = snapshot;
  m_Options = none;

#if 0
  m_lOpen = AFX_RECORDSET_STATUS_UNKNOWN;
  m_nEditMode = noMode;
  m_nDefaultType = snapshot;

  m_bAppendable = false;
  m_bUpdatable = false;
  m_bScrollable = false;
  m_bRecordsetDb = false;
  m_bRebindParams = false;
  m_bLongBinaryColumns = false;
  m_nLockMode = optimistic;
  m_dwInitialGetDataLen = 0;
  m_rgODBCFieldInfos = NULL;
  m_rgFieldInfos = NULL;
  m_rgRowStatus = NULL;
  m_dwRowsetSize = 25;
  m_dwAllocatedRowsetSize = 0;

  m_nFields = 0;
  m_nParams = 0;
  m_nFieldsBound = 0;
  m_lCurrentRecord = AFX_CURRENT_RECORD_UNDEFINED;
  m_lRecordCount = 0;
  m_bUseUpdateSQL = false;
  m_bUseODBCCursorLib = false;
  m_nResultCols = -1;
  m_bCheckCacheForDirtyFields = true;

  m_pbFieldFlags = NULL;
  m_pbParamFlags = NULL;
  m_plParamLength = NULL;
  m_pvFieldProxy = NULL;
  m_pvParamProxy = NULL;
  m_nProxyFields = 0;
  m_nProxyParams = 0;

  m_hstmtUpdate = SQL_NULL_HSTMT;
#endif // 0
  } // end of RECSET constructor

RECSET::~RECSET()
  {
  try {
    if (m_hstmt) {
#ifdef DEBTRACE
 if (m_dwOptions & useMultiRowFetch) {
  htrc("WARNING: Close called implicitly from destructor\n");
  htrc("Use of multi row fetch requires explicit call\n");
  htrc("to Close or memory leaks will result\n");
  }
#endif
      Close();
      } // endif m_hstmt

//  if (m_bRecordsetDb)
//    delete m_pDB;                  ??????

    m_pDB = NULL;
  } catch(DBX*) {
    // Nothing we can do
#ifdef DEBTRACE
 htrc("Error: Exception ignored in ~RECSET\n");
#endif
  } // endtry/catch

  } // end of ~RECSET

/***********************************************************************/
/*  Open: this function does the following:                            */
/*        Allocates the hstmt,                                         */
/*        Bind columns,                                                */
/*        Execute the SQL statement                                    */
/***********************************************************************/
bool RECSET::Open(PSZ sql, uint Type, DWORD options)
  {
  ASSERT(m_pDB && m_pDB->IsOpen());
  ASSERT(Type == DB_USE_DEFAULT_TYPE || Type == dynaset ||
         Type == snapshot || Type == forwardOnly || Type == dynamic);
//ASSERT(!(options & readOnly && options & appendOnly));

  // Cache state info and allocate hstmt
  SetState(Type, sql, options);

  try {
    if (m_hstmt) {
      if (IsOpen())
        Close(SQL_CLOSE);

    } else {
      RETCODE rc = SQLAllocStmt(m_pDB->m_hdbc, &m_hstmt);

      if (!Check(rc))
        ThrowDBException(SQL_INVALID_HANDLE);

    } // endif m_hstmt

    m_pDB->OnSetOptions(m_hstmt);

    // Allocate the field/param status arrays, if necessary
//  bool bUnbound = false;

//  if (m_nFields > 0 || m_nParams > 0)
//    AllocStatusArrays();
//  else
//    bUnbound = true;

    // Build SQL and prep/execute or just execute direct
//  BuildSQL(sql);
    PrepareAndExecute(sql);

    // Cache some field info and prepare the rowset
    AllocAndCacheFieldInfo();
    AllocRowset();

    // If late binding, still need to allocate status arrays
//  if (bUnbound && (m_nFields > 0 || m_nParams > 0))
//    AllocStatusArrays();

  } catch(DBX *x) {
    Close(SQL_DROP);
//    strcpy(m_pDB->m_G->Message, x->GetErrorMessage[0]);
    strcpy(m_pDB->m_G->Message, x->GetErrorMessage(0));
    return true;
  } // endtry/catch

  return false;
  } // end of Open

/***********************************************************************/
/*  Close a hstmt.                                                     */
/***********************************************************************/
void RECSET::Close(SWORD option)
  {
  if (m_hstmt != SQL_NULL_HSTMT) {
    RETCODE rc = SQLFreeStmt(m_hstmt, option);

    if (option == SQL_DROP)
      m_hstmt = SQL_NULL_HSTMT;

    } // endif m_hstmt

#if 0
  m_lOpen = RECORDSET_STATUS_CLOSED;
  m_bBOF = true;
  m_bEOF = true;
  m_bDeleted = false;
  m_bAppendable = false;
  m_bUpdatable = false;
  m_bScrollable = false;
  m_bRebindParams = false;
  m_bLongBinaryColumns = false;
  m_nLockMode = optimistic;
  m_nFieldsBound = 0;
  m_nResultCols = -1;
#endif // 0
  } // end of Close
#endif // 0

