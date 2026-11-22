/* The C modules in src/Modules, as seen from src/main.cpp. */
#ifndef LEGACY_MODULES_H
#define LEGACY_MODULES_H

#ifdef __cplusplus
extern "C" {
#endif

/* QueryApiImpl for the C modules in src/Modules; REGDB_E_CLASSNOTREG when none of them has clsid. */
HRESULT modules_query_api( const GUID *clsid, const GUID *iid, void **out );

#ifdef __cplusplus
}
#endif

#endif
